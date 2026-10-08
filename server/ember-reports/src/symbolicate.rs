//! Local symbols only. No symbol servers, registers, environment or memory in events.
use crate::limits::Ticket;
use async_trait::async_trait;
use breakpad_symbols::{
    FileError, FileKind, LocateSymbolsResult, SimpleSymbolSupplier, SymbolError, SymbolFile,
    SymbolSupplier, Symbolizer,
};
use minidump::{Minidump, MinidumpModuleList, Module};
use minidump_unwind::{FillSymbolError, FrameSymbolizer, FrameWalker, SymbolProvider};
use std::{
    io::Read,
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};

pub const FRAMES: usize = 64;
pub const THREADS: usize = 128;
const SYMBOL_BYTES: usize = 8 * 1024 * 1024;
const TOTAL_SYMBOL_BYTES: usize = 16 * 1024 * 1024;

#[derive(Clone, Default)]
pub struct Frame {
    pub instruction: u64,
    pub module: String,
    pub offset: u64,
    pub function: Option<String>,
    pub filename: Option<String>,
    pub line: Option<u32>,
}
#[derive(Default)]
pub struct Thread {
    pub id: u32,
    pub frames: Vec<Frame>,
}
#[derive(Default)]
pub struct Walk {
    pub code: Option<u32>,
    pub reason: Option<String>,
    pub address: Option<u64>,
    pub crashing_thread: Option<u32>,
    pub crash_frames: Vec<Frame>,
    pub threads: Vec<Thread>,
    pub status: &'static str,
}

pub fn basename(path: &str) -> &str {
    path.rsplit(['/', '\\']).next().unwrap_or(path)
}
pub fn head(s: &str, max: usize) -> String {
    let mut end = s.len().min(max);
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    s[..end].to_owned()
}
pub fn exception_name(code: u32, operation: Option<u64>) -> String {
    match (code, operation) {
        (0xc0000005, Some(0)) => "EXCEPTION_ACCESS_VIOLATION_READ".into(),
        (0xc0000005, Some(1)) => "EXCEPTION_ACCESS_VIOLATION_WRITE".into(),
        (0xc0000005, Some(8)) => "EXCEPTION_ACCESS_VIOLATION_EXEC".into(),
        (0xc0000005, _) => "EXCEPTION_ACCESS_VIOLATION".into(),
        (0xc0000006, _) => "EXCEPTION_IN_PAGE_ERROR".into(),
        (0xc000001d, _) => "EXCEPTION_ILLEGAL_INSTRUCTION".into(),
        (0xc0000094, _) => "EXCEPTION_INT_DIVIDE_BY_ZERO".into(),
        (0xc00000fd, _) => "EXCEPTION_STACK_OVERFLOW".into(),
        (0x80000003, _) => "EXCEPTION_BREAKPOINT".into(),
        (0xc0000409, _) => "STATUS_STACK_BUFFER_OVERRUN".into(),
        _ => format!("0x{code:08x}"),
    }
}

fn u32_at(bytes: &[u8], start: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        bytes.get(start..start.checked_add(4)?)?.try_into().ok()?,
    ))
}
fn u64_at(bytes: &[u8], start: usize) -> Option<u64> {
    Some(u64::from_le_bytes(
        bytes.get(start..start.checked_add(8)?)?.try_into().ok()?,
    ))
}
fn directory(bytes: &[u8]) -> Option<Vec<(u32, usize, usize)>> {
    if !bytes.starts_with(b"MDMP") || bytes.len() > crate::intake::DUMP_BYTES {
        return None;
    }
    let count = u32_at(bytes, 8)? as usize;
    let start = u32_at(bytes, 12)? as usize;
    if count > 64 {
        return None;
    }
    let mut entries = Vec::new();
    for i in 0..count {
        let p = start.checked_add(i * 12)?;
        let kind = u32_at(bytes, p)?;
        let size = u32_at(bytes, p + 4)? as usize;
        let offset = u32_at(bytes, p + 8)? as usize;
        bytes.get(offset..offset.checked_add(size)?)?;
        entries.push((kind, size, offset));
    }
    Some(entries)
}

// Read only the fixed exception record before the expensive walk. Even a
// corrupt/missing system or thread stream still yields the raw crash facts.
fn raw_facts(bytes: &[u8]) -> Walk {
    let mut result = Walk {
        status: "invalid_dump",
        ..Walk::default()
    };
    if let Some(entries) = directory(bytes)
        && let Some((_, size, offset)) = entries.iter().find(|e| e.0 == 6)
        && *size >= 168
    {
        let p = *offset;
        result.code = u32_at(bytes, p + 8);
        result.crashing_thread = u32_at(bytes, p);
        let params = u32_at(bytes, p + 32).unwrap_or(0);
        let operation = if params >= 1 {
            u64_at(bytes, p + 40)
        } else {
            None
        };
        result.reason = result.code.map(|code| exception_name(code, operation));
        result.address = if matches!(result.code, Some(0xc0000005 | 0xc0000006)) && params >= 2 {
            u64_at(bytes, p + 48)
        } else {
            u64_at(bytes, p + 24)
        };
        if let Some(instruction) = u64_at(bytes, p + 24) {
            result.crash_frames.push(Frame {
                instruction,
                module: "unknown".into(),
                offset: instruction,
                ..Frame::default()
            });
        }
    }
    result
}

// Keep only the streams needed to walk Windows stacks. Bound entry counts and
// module strings before rust-minidump allocates them; offsets still reference
// the original bytes. Crashpad annotations and arbitrary extension streams are
// deliberately excluded from the processor's view (the raw dump stays intact).
fn bounded_dump(mut bytes: Vec<u8>) -> Option<Vec<u8>> {
    let entries = directory(&bytes)?;
    let mut kept = Vec::new();
    for (kind, size, offset) in entries {
        if !matches!(kind, 3..=7 | 9) {
            continue;
        }
        if kept.iter().any(|e: &(u32, usize, usize)| e.0 == kind) {
            return None;
        }
        if matches!(kind, 3..=5) {
            let count = u32_at(&bytes, offset)? as usize;
            let (cap, item) = match kind {
                3 => (THREADS, 48),
                4 => (256, 108),
                _ => (1024, 16),
            };
            if count > cap || size < 4 + count * item {
                return None;
            }
            if kind == 4 {
                for i in 0..count {
                    let p = offset + 4 + i * item;
                    let name = u32_at(&bytes, p + 20)? as usize;
                    if u32_at(&bytes, name)? > 1024 || u32_at(&bytes, p + 76)? > 1024 {
                        return None;
                    }
                }
            }
        }
        if kind == 9 && (size < 16 || u64_at(&bytes, offset)? > 1024) {
            return None;
        }
        if kind == 6 && (size < 168 || u32_at(&bytes, offset + 32)? > 15) {
            return None;
        }
        kept.push((kind, size, offset));
    }
    let new_directory = bytes.len() as u32;
    bytes
        .get_mut(8..12)?
        .copy_from_slice(&(kept.len() as u32).to_le_bytes());
    bytes
        .get_mut(12..16)?
        .copy_from_slice(&new_directory.to_le_bytes());
    for (kind, size, offset) in kept {
        bytes.extend_from_slice(&kind.to_le_bytes());
        bytes.extend_from_slice(&(size as u32).to_le_bytes());
        bytes.extend_from_slice(&(offset as u32).to_le_bytes());
    }
    Some(bytes)
}

struct LocalSymbols {
    root: PathBuf,
    local: SimpleSymbolSupplier,
    bytes: AtomicUsize,
}
#[async_trait]
impl SymbolSupplier for LocalSymbols {
    async fn locate_symbols(
        &self,
        module: &(dyn Module + Sync),
    ) -> Result<LocateSymbolsResult, SymbolError> {
        // Validate leaf names before invoking the local-path supplier.
        let lookup = breakpad_symbols::breakpad_sym_lookup(module).ok_or(SymbolError::NotFound)?;
        if lookup
            .cache_rel
            .split('/')
            .any(|s| s.is_empty() || s == "." || s == ".." || s.contains(['\\', ':', '\0']))
        {
            return Err(SymbolError::NotFound);
        }
        let path = self
            .local
            .locate_file(module, FileKind::BreakpadSym)
            .await
            .map_err(|_| SymbolError::NotFound)?;
        let path = path.canonicalize().map_err(|_| SymbolError::NotFound)?;
        if !path.starts_with(&self.root) {
            return Err(SymbolError::NotFound);
        }
        let file = std::fs::File::open(path).map_err(|_| SymbolError::NotFound)?;
        let length = file.metadata().map_err(|_| SymbolError::NotFound)?.len();
        if length > SYMBOL_BYTES as u64 {
            return Err(SymbolError::NotFound);
        }
        if self.bytes.fetch_add(length as usize, Ordering::Relaxed) + length as usize
            > TOTAL_SYMBOL_BYTES
        {
            return Err(SymbolError::NotFound);
        }
        let mut bytes = Vec::new();
        file.take((SYMBOL_BYTES + 1) as u64)
            .read_to_end(&mut bytes)
            .map_err(|_| SymbolError::NotFound)?;
        if bytes.len() > SYMBOL_BYTES {
            return Err(SymbolError::NotFound);
        }
        let symbols = SymbolFile::from_bytes(&bytes)?;
        tokio::task::yield_now().await;
        Ok(LocateSymbolsResult {
            symbols,
            extra_debug_info: None,
        })
    }
    async fn locate_file(
        &self,
        _: &(dyn Module + Sync),
        _: FileKind,
    ) -> Result<PathBuf, FileError> {
        Err(FileError::NotFound)
    }
}

struct Cooperative {
    symbols: Symbolizer,
    operations: AtomicUsize,
}
impl Cooperative {
    async fn checkpoint(&self) {
        // Returning errors alone would allow stack scanning to keep running.
        // Pending stops the walk until the enclosing deadline drops the future.
        if self.operations.fetch_add(1, Ordering::Relaxed) >= 8192 {
            std::future::pending::<()>().await;
        }
        tokio::task::yield_now().await;
    }
}
#[async_trait]
impl SymbolProvider for Cooperative {
    async fn fill_symbol(
        &self,
        module: &(dyn Module + Sync),
        frame: &mut (dyn FrameSymbolizer + Send),
    ) -> Result<(), FillSymbolError> {
        self.checkpoint().await;
        self.symbols.fill_symbol(module, frame).await
    }
    async fn walk_frame(
        &self,
        module: &(dyn Module + Sync),
        walker: &mut (dyn FrameWalker + Send),
    ) -> Option<()> {
        self.checkpoint().await;
        self.symbols.walk_frame(module, walker).await
    }
    async fn get_file_path(
        &self,
        _: &(dyn Module + Sync),
        _: FileKind,
    ) -> Result<PathBuf, FileError> {
        Err(FileError::NotFound)
    }
}

pub async fn walk(
    bytes: Vec<u8>,
    symbols: PathBuf,
    deadline: Duration,
    ticket: Option<Arc<Ticket>>,
) -> Walk {
    let fallback = raw_facts(&bytes);
    tokio::task::spawn_blocking(move || {
        let _ticket = ticket;
        let started = Instant::now();
        let mut raw = raw_facts(&bytes);
        let Some(bytes) = bounded_dump(bytes) else {
            raw.status = "input_budget";
            return raw;
        };
        let Ok(dump) = Minidump::read(bytes) else {
            return raw;
        };
        // Resolve the exception instruction independently of the processor, so
        // missing thread/system streams do not lose module+offset grouping.
        if let Ok(modules) = dump.get_stream::<MinidumpModuleList>()
            && let Some(top) = raw.crash_frames.first_mut()
            && let Some(module) = modules.module_at_address(top.instruction)
        {
            top.module = head(basename(&module.code_file()), 128);
            top.offset = top.instruction.saturating_sub(module.base_address());
        }
        let root = symbols.canonicalize().unwrap_or(symbols);
        let provider = Cooperative {
            symbols: Symbolizer::new(LocalSymbols {
                local: SimpleSymbolSupplier::new(vec![root.clone()]),
                root,
                bytes: AtomicUsize::new(0),
            }),
            operations: AtomicUsize::new(0),
        };
        let Ok(runtime) = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
        else {
            raw.status = "runtime_failed";
            return raw;
        };
        let processed = runtime.block_on(async {
            tokio::time::timeout(
                deadline.saturating_sub(started.elapsed()),
                minidump_processor::process_minidump(&dump, &provider),
            )
            .await
        });
        match processed {
            Ok(Ok(state)) => {
                for (index, thread) in state.threads.iter().take(THREADS).enumerate() {
                    let frames = thread
                        .frames
                        .iter()
                        .take(FRAMES)
                        .map(|f| {
                            let (module, base) =
                                f.module.as_ref().map_or(("unknown".into(), 0), |m| {
                                    (head(basename(&m.code_file()), 128), m.base_address())
                                });
                            Frame {
                                instruction: f.instruction,
                                module,
                                offset: f.instruction.saturating_sub(base),
                                function: f.function_name.as_deref().map(|s| head(s, 256)),
                                filename: f
                                    .source_file_name
                                    .as_deref()
                                    .map(|s| head(basename(s), 256)),
                                line: f.source_line,
                            }
                        })
                        .collect::<Vec<_>>();
                    if raw.crashing_thread == Some(thread.thread_id)
                        || (raw.crashing_thread.is_none() && state.requesting_thread == Some(index))
                    {
                        raw.crashing_thread = Some(thread.thread_id);
                        if !frames.is_empty() {
                            raw.crash_frames = frames;
                        }
                    } else {
                        raw.threads.push(Thread {
                            id: thread.thread_id,
                            frames,
                        });
                    }
                }
                raw.status = "ok";
            }
            Ok(Err(_)) => raw.status = "walk_failed",
            Err(_) => raw.status = "timeout",
        }
        raw
    })
    .await
    .unwrap_or(Walk {
        status: "worker_failed",
        ..fallback
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn exhausted_operation_budget_yields_to_deadline() {
        let provider = Cooperative {
            symbols: Symbolizer::new(SimpleSymbolSupplier::new(Vec::new())),
            operations: AtomicUsize::new(8192),
        };
        assert!(
            tokio::time::timeout(Duration::from_millis(5), provider.checkpoint())
                .await
                .is_err()
        );
    }
}
