//! Local symbols only. No symbol servers, registers, environment or memory in events.
use async_trait::async_trait;
use breakpad_symbols::{
    FileError, FileKind, LocateSymbolsResult, SymbolError, SymbolFile, SymbolSupplier, Symbolizer,
};
use minidump::{Minidump, MinidumpModuleList, Module};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    io::{self, Read},
    path::PathBuf,
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
};

pub const FRAMES: usize = 64;
pub const THREADS: usize = 128;
// Sidecar.sym, with file and line records, measured 29.8 MiB at 1.5.0
// (dump_syms 2.3.9); the cap leaves it room to double. A walk reads Sidecar's
// and its DLLs' files together. The worker's memory (worker.rs) holds a parse
// of the largest.
pub const SYMBOL_BYTES: usize = 64 * 1024 * 1024;
const TOTAL_SYMBOL_BYTES: usize = 96 * 1024 * 1024;

#[derive(Clone, Default, Deserialize, Serialize)]
pub struct Frame {
    pub instruction: u64,
    pub module: String,
    pub offset: u64,
    pub function: Option<String>,
    pub filename: Option<String>,
    pub line: Option<u32>,
}
#[derive(Default, Deserialize, Serialize)]
pub struct Thread {
    pub id: u32,
    pub frames: Vec<Frame>,
}
/// How far symbolication got. Symbol problems are kept as separate detail.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum WalkStatus {
    Ok,
    // An empty walk describes a report that carried no dump.
    #[default]
    NoDump,
    InvalidDump,
    InputBudget,
    WalkFailed,
    WorkerFailed,
    WorkerUnsupported,
    Timeout,
}
impl WalkStatus {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Ok => "ok",
            Self::NoDump => "no_dump",
            Self::InvalidDump => "invalid_dump",
            Self::InputBudget => "input_budget",
            Self::WalkFailed => "walk_failed",
            Self::WorkerFailed => "worker_failed",
            Self::WorkerUnsupported => "worker_unsupported",
            Self::Timeout => "timeout",
        }
    }
    fn parse(label: &str) -> Option<Self> {
        [
            Self::Ok,
            Self::NoDump,
            Self::InvalidDump,
            Self::InputBudget,
            Self::WalkFailed,
            Self::WorkerFailed,
            Self::WorkerUnsupported,
            Self::Timeout,
        ]
        .into_iter()
        .find(|status| status.as_str() == label)
    }
}
#[derive(Default)]
pub struct Walk {
    pub code: Option<u32>,
    pub reason: Option<String>,
    pub address: Option<u64>,
    pub crashing_thread: Option<u32>,
    pub crash_frames: Vec<Frame>,
    pub threads: Vec<Thread>,
    pub status: WalkStatus,
    // Missing or unusable symbols, shown in the event's symbolication text.
    pub symbol_problems: Option<String>,
    // Worker-to-parent diagnostics only; event::build does not export this field.
    pub unreadable_symbols: Vec<String>,
}
impl Walk {
    /// The `symbolication` value in the event: the status, or with symbol
    /// problems "partial: <problems>" after a full walk and
    /// "<status>: <problems>" otherwise.
    pub fn symbolication(&self) -> String {
        match (&self.symbol_problems, self.status) {
            (None, status) => status.as_str().to_owned(),
            (Some(problems), WalkStatus::Ok) => format!("partial: {problems}"),
            (Some(problems), status) => format!("{}: {problems}", status.as_str()),
        }
    }
    // Inverse of symbolication(). Labels never contain ": ".
    fn from_symbolication(text: &str) -> Option<(WalkStatus, Option<String>)> {
        match text.split_once(": ") {
            None => Some((WalkStatus::parse(text)?, None)),
            Some(("partial", problems)) => Some((WalkStatus::Ok, Some(problems.to_owned()))),
            Some((label, problems)) => Some((WalkStatus::parse(label)?, Some(problems.to_owned()))),
        }
    }
}

// The worker writes this JSON to its parent. The binary is replaced while an
// older parent may still run and launch it, so `status` keeps the combined
// symbolication text both versions understand.
#[derive(Serialize)]
struct WalkOut<'a> {
    code: Option<u32>,
    reason: &'a Option<String>,
    address: Option<u64>,
    crashing_thread: Option<u32>,
    crash_frames: &'a [Frame],
    threads: &'a [Thread],
    status: String,
    #[serde(skip_serializing_if = "<[String]>::is_empty")]
    unreadable_symbols: &'a [String],
}
#[derive(Deserialize)]
struct WalkIn {
    code: Option<u32>,
    reason: Option<String>,
    address: Option<u64>,
    crashing_thread: Option<u32>,
    crash_frames: Vec<Frame>,
    threads: Vec<Thread>,
    status: String,
    #[serde(default)]
    unreadable_symbols: Vec<String>,
}
impl Serialize for Walk {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        WalkOut {
            code: self.code,
            reason: &self.reason,
            address: self.address,
            crashing_thread: self.crashing_thread,
            crash_frames: &self.crash_frames,
            threads: &self.threads,
            status: self.symbolication(),
            unreadable_symbols: &self.unreadable_symbols,
        }
        .serialize(serializer)
    }
}
impl<'de> Deserialize<'de> for Walk {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let wire = WalkIn::deserialize(deserializer)?;
        let (status, symbol_problems) = Walk::from_symbolication(&wire.status)
            .ok_or_else(|| serde::de::Error::custom("unknown walk status"))?;
        Ok(Self {
            code: wire.code,
            reason: wire.reason,
            address: wire.address,
            crashing_thread: wire.crashing_thread,
            crash_frames: wire.crash_frames,
            threads: wire.threads,
            status,
            symbol_problems,
            unreadable_symbols: wire.unreadable_symbols,
        })
    }
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
pub(crate) fn raw_facts(bytes: &[u8]) -> Walk {
    let mut result = Walk {
        status: WalkStatus::InvalidDump,
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

// The launcher's live-stack stream (src/common/CrashStackSupplement.hxx):
// version, count, {thread id, start, size} per range, then the range bytes.
// DbgHelp under Wine stores a thread's outer stack only when its stack pointer
// is below the TEB's StackLimit; the range here is the missing part up to it.
const STACK_SUPPLEMENT: u32 = 0x5346_3401;
const STACK_SUPPLEMENT_BYTES: usize = 2 * 1024 * 1024;

// One thread's stack, replaced: MINIDUMP_THREAD record offset, new start and
// the bytes the descriptor will point at.
struct StackEdit {
    record: usize,
    start: u32,
    blob: Vec<u8>,
}

// Plan, for each listed thread, the stack its descriptor should point at, so
// the walk starts at the stack pointer. One rule, by the thread's stored stack
// against the range [start, end):
// - stored starts at or below the range: DbgHelp already kept esp; leave it;
// - stored starts inside or at the end of the range: the bytes before the
//   stored start, then the stored stack (adjacent and overlapping alike);
// - a gap: the range alone;
// and in the last two only if the context's esp lies in the range, or in the
// range and the stored stack, so a range read at another moment than the
// context is never used (the record stays).
// Hostile dumps are bounded: no empty ranges, one edit per thread, and all
// appended bytes within DUMP_BYTES. Any malformed field refuses the plan.
fn plan_stack_splice(
    bytes: &[u8],
    thread_list: usize,
    stream_offset: usize,
    stream_size: usize,
) -> Option<Vec<StackEdit>> {
    if u32_at(bytes, stream_offset)? != 1 {
        return None;
    }
    let count = u32_at(bytes, stream_offset + 4)? as usize;
    if count > THREADS {
        return None;
    }
    let table = stream_offset.checked_add(8)?;
    let mut data = table.checked_add(count * 12)?;
    let end = stream_offset.checked_add(stream_size)?;
    let thread_count = u32_at(bytes, thread_list)? as usize;
    let mut edits: Vec<StackEdit> = Vec::new();
    let (mut supplied, mut appended) = (0usize, 0usize);
    for i in 0..count {
        let id = u32_at(bytes, table + i * 12)?;
        let range_start = u32_at(bytes, table + i * 12 + 4)?;
        let range_size = u32_at(bytes, table + i * 12 + 8)? as usize;
        supplied = supplied.checked_add(range_size)?;
        let next = data.checked_add(range_size)?;
        if range_size == 0 || supplied > STACK_SUPPLEMENT_BYTES || next > end {
            return None;
        }
        let supplement = bytes.get(data..next)?;
        data = next;
        let range_end = u64::from(range_start) + range_size as u64;
        for t in 0..thread_count {
            let record = thread_list + 4 + t * 48;
            if u32_at(bytes, record)? != id {
                continue;
            }
            if edits.iter().any(|e| e.record == record) {
                break;
            }
            let stored = u64_at(bytes, record + 24)?;
            let stored_size = u32_at(bytes, record + 32)? as usize;
            let stored_rva = u32_at(bytes, record + 36)? as usize;
            let stored_bytes = bytes.get(stored_rva..stored_rva.checked_add(stored_size)?)?;
            if stored <= u64::from(range_start) {
                break;
            }
            // x86 CONTEXT.Esp is at 0xC4, in the record's context.
            let context = u32_at(bytes, record + 44)? as usize;
            let esp = u64::from(u32_at(bytes, context.checked_add(0xC4)?)?);
            let mut blob = Vec::new();
            if stored <= range_end {
                // A range from another moment than the context is never joined.
                if esp < u64::from(range_start) || esp >= stored + stored_size as u64 {
                    break;
                }
                blob.extend_from_slice(&supplement[..(stored - u64::from(range_start)) as usize]);
                blob.extend_from_slice(stored_bytes);
            } else {
                if esp < u64::from(range_start) || esp >= range_end {
                    break;
                }
                blob.extend_from_slice(supplement);
            }
            appended = appended.checked_add(blob.len())?;
            if appended > crate::intake::DUMP_BYTES {
                return None;
            }
            edits.push(StackEdit {
                record,
                start: range_start,
                blob,
            });
            break;
        }
    }
    Some(edits)
}

// Every field written was read by the plan, so this cannot fail.
fn apply_stack_splice(bytes: &mut Vec<u8>, edits: Vec<StackEdit>) {
    for edit in edits {
        let rva = (bytes.len() as u32).to_le_bytes();
        let len = (edit.blob.len() as u32).to_le_bytes();
        bytes.extend_from_slice(&edit.blob);
        bytes[edit.record + 24..edit.record + 32]
            .copy_from_slice(&u64::from(edit.start).to_le_bytes());
        bytes[edit.record + 32..edit.record + 36].copy_from_slice(&len);
        bytes[edit.record + 36..edit.record + 40].copy_from_slice(&rva);
    }
}

// Keep only the streams needed to walk Windows stacks. Bound entry counts and
// module strings before rust-minidump allocates them; offsets reference the
// original bytes, except thread stacks the launcher's stack supplement
// (0x53463401) is joined into, which point at bytes appended after them.
// Crashpad annotations and other extension streams are deliberately excluded
// from the processor's view (the raw dump stays intact).
fn bounded_dump(mut bytes: Vec<u8>) -> Option<Vec<u8>> {
    let entries = directory(&bytes)?;
    let mut kept = Vec::new();
    let mut supplement = None;
    for (kind, size, offset) in entries {
        if kind == STACK_SUPPLEMENT && supplement.is_none() {
            supplement = Some((size, offset));
        }
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
            // rust-minidump also accepts four padding bytes before records.
            // Support only the exact unpadded layout, never ambiguous offsets.
            if count > cap || size != 4 + count * item {
                return None;
            }
            if kind == 4 {
                for i in 0..count {
                    let p = offset + 4 + i * item;
                    let name = u32_at(&bytes, p + 20)? as usize;
                    let name_bytes = u32_at(&bytes, name)? as usize;
                    let cv_bytes = u32_at(&bytes, p + 76)? as usize;
                    let cv = u32_at(&bytes, p + 80)? as usize;
                    if name_bytes > 1024 || !name_bytes.is_multiple_of(2) || cv_bytes > 1024 {
                        return None;
                    }
                    let name_start = name.checked_add(4)?;
                    bytes.get(name_start..name_start.checked_add(name_bytes)?)?;
                    bytes.get(cv..cv.checked_add(cv_bytes)?)?;
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
    if let Some((size, offset)) = supplement
        && let Some(threads) = kept.iter().find(|e| e.0 == 3)
        && let Some(edits) = plan_stack_splice(&bytes, threads.2, offset, size)
    {
        apply_stack_splice(&mut bytes, edits);
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

#[derive(Clone, Copy, PartialEq, Eq)]
enum SymbolProblem {
    Missing,
    Unreadable,
    Rejected,
    Budget,
    Corrupt,
}
impl SymbolProblem {
    fn label(self) -> &'static str {
        match self {
            Self::Missing => "missing",
            Self::Unreadable => "unreadable",
            Self::Rejected => "path rejected",
            Self::Budget => "over budget",
            Self::Corrupt => "corrupt",
        }
    }
}

// Never allow paths or control characters from the dump into a diagnostic.
pub(crate) fn safe_symbol_relative(name: &str) -> bool {
    name.len() <= 2048
        && !name.chars().any(char::is_control)
        && name.split('/').count() == 3
        && name
            .split('/')
            .all(|s| !s.is_empty() && s != "." && s != ".." && !s.contains(['\\', ':']))
}

#[derive(Default)]
struct SymbolProblems(Mutex<BTreeMap<String, SymbolProblem>>);
impl SymbolProblems {
    fn record(&self, name: &str, problem: SymbolProblem) {
        self.0.lock().unwrap().insert(name.to_owned(), problem);
    }
    fn apply(&self, walk: &mut Walk) {
        let problems = self.0.lock().unwrap();
        if problems.is_empty() {
            return;
        }
        walk.symbol_problems = Some(
            problems
                .iter()
                .map(|(name, problem)| {
                    format!("symbols {} for {}", problem.label(), head(name, 256))
                })
                .collect::<Vec<_>>()
                .join("; "),
        );
        walk.unreadable_symbols = problems
            .iter()
            .filter(|(name, problem)| {
                **problem == SymbolProblem::Unreadable && safe_symbol_relative(name)
            })
            .map(|(name, _)| name.clone())
            .collect();
    }
}

struct LocalSymbols {
    root: PathBuf,
    bytes: AtomicUsize,
    problems: Arc<SymbolProblems>,
}
impl LocalSymbols {
    fn io_error(&self, name: &str, error: io::Error) -> SymbolError {
        self.problems.record(
            name,
            if error.kind() == io::ErrorKind::NotFound {
                SymbolProblem::Missing
            } else {
                SymbolProblem::Unreadable
            },
        );
        SymbolError::NotFound
    }
}
#[async_trait]
impl SymbolSupplier for LocalSymbols {
    async fn locate_symbols(
        &self,
        module: &(dyn Module + Sync),
    ) -> Result<LocateSymbolsResult, SymbolError> {
        // Validate leaf names before accessing the local store.
        let module_name = head(basename(&module.code_file()), 128)
            .chars()
            .map(|c| if c.is_control() { '?' } else { c })
            .collect::<String>();
        let Some(lookup) = breakpad_symbols::breakpad_sym_lookup(module) else {
            self.problems.record(&module_name, SymbolProblem::Missing);
            return Err(SymbolError::NotFound);
        };
        let name = &lookup.cache_rel;
        if !safe_symbol_relative(name) {
            self.problems.record(&module_name, SymbolProblem::Rejected);
            return Err(SymbolError::NotFound);
        }
        // Check the expected store path directly: SimpleSymbolSupplier's
        // metadata().ok() would collapse permission failures into missing.
        let path = self
            .root
            .join(name)
            .canonicalize()
            .map_err(|error| self.io_error(name, error))?;
        if !path.starts_with(&self.root) {
            self.problems.record(name, SymbolProblem::Rejected);
            return Err(SymbolError::NotFound);
        }
        if !path
            .metadata()
            .map_err(|error| self.io_error(name, error))?
            .is_file()
        {
            self.problems.record(name, SymbolProblem::Rejected);
            return Err(SymbolError::NotFound);
        }
        let file = std::fs::File::open(path).map_err(|error| self.io_error(name, error))?;
        let metadata = file
            .metadata()
            .map_err(|error| self.io_error(name, error))?;
        if !metadata.is_file() {
            self.problems.record(name, SymbolProblem::Rejected);
            return Err(SymbolError::NotFound);
        }
        let length = metadata.len();
        if length > SYMBOL_BYTES as u64
            || self.bytes.fetch_add(length as usize, Ordering::Relaxed) + length as usize
                > TOTAL_SYMBOL_BYTES
        {
            self.problems.record(name, SymbolProblem::Budget);
            return Err(SymbolError::NotFound);
        }
        let mut bytes = Vec::new();
        file.take((SYMBOL_BYTES + 1) as u64)
            .read_to_end(&mut bytes)
            .map_err(|error| self.io_error(name, error))?;
        if bytes.len() > SYMBOL_BYTES {
            self.problems.record(name, SymbolProblem::Budget);
            return Err(SymbolError::NotFound);
        }
        let symbols = SymbolFile::from_bytes(&bytes).inspect_err(|_| {
            self.problems.record(name, SymbolProblem::Corrupt);
        })?;
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

// Called only by the isolated symbolicate process. Its runtime is the process's
// sole runtime; the parent owns all deadlines and OS resource limits.
pub(crate) async fn walk(bytes: Vec<u8>, symbols: PathBuf) -> Walk {
    let mut raw = raw_facts(&bytes);
    let Some(bytes) = bounded_dump(bytes) else {
        raw.status = WalkStatus::InputBudget;
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
    let problems = Arc::new(SymbolProblems::default());
    let provider = Symbolizer::new(LocalSymbols {
        root,
        bytes: AtomicUsize::new(0),
        problems: problems.clone(),
    });
    match minidump_processor::process_minidump(&dump, &provider).await {
        Ok(state) => {
            for (index, thread) in state.threads.iter().take(THREADS).enumerate() {
                let frames = thread
                    .frames
                    .iter()
                    .take(FRAMES)
                    .map(|f| {
                        let (module, base) = f.module.as_ref().map_or(("unknown".into(), 0), |m| {
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
            raw.status = WalkStatus::Ok;
        }
        Err(_) => raw.status = WalkStatus::WalkFailed,
    }
    problems.apply(&mut raw);
    raw
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn io_problems_are_distinct_and_preserve_walk_failure() {
        let problems = Arc::new(SymbolProblems::default());
        let supplier = LocalSymbols {
            root: PathBuf::from("unused"),
            bytes: AtomicUsize::new(0),
            problems: problems.clone(),
        };
        let unreadable = "Launcher.pdb/ABC1/Launcher.sym";
        supplier.io_error(unreadable, io::Error::from(io::ErrorKind::PermissionDenied));
        supplier.io_error(unreadable, io::Error::from(io::ErrorKind::PermissionDenied));
        supplier.io_error(
            "SSFIV.exe/DEF1/SSFIV.exe.sym",
            io::Error::from(io::ErrorKind::NotFound),
        );
        let mut walk = Walk {
            status: WalkStatus::Ok,
            ..Walk::default()
        };
        problems.apply(&mut walk);
        assert_eq!(
            walk.symbolication(),
            "partial: symbols unreadable for Launcher.pdb/ABC1/Launcher.sym; symbols missing for SSFIV.exe/DEF1/SSFIV.exe.sym"
        );
        assert_eq!(walk.unreadable_symbols, [unreadable]);
        walk.status = WalkStatus::WalkFailed;
        problems.apply(&mut walk);
        assert!(
            walk.symbolication()
                .starts_with("walk_failed: symbols unreadable for ")
        );
    }

    // The worker JSON as both sides read and wrote it before WalkStatus.
    #[derive(Default, Deserialize, Serialize)]
    struct LegacyWalk {
        code: Option<u32>,
        reason: Option<String>,
        address: Option<u64>,
        crashing_thread: Option<u32>,
        crash_frames: Vec<Frame>,
        threads: Vec<Thread>,
        status: String,
        #[serde(default, skip_serializing_if = "Vec::is_empty")]
        unreadable_symbols: Vec<String>,
    }
    fn compatibility_cases() -> Vec<(WalkStatus, Option<&'static str>, &'static str)> {
        let missing = "symbols missing for SSFIV.exe/DEF1/SSFIV.exe.sym";
        let both = "symbols unreadable for Launcher.pdb/ABC1/Launcher.sym; symbols missing for SSFIV.exe/DEF1/SSFIV.exe.sym";
        vec![
            (WalkStatus::Ok, None, "ok"),
            (
                WalkStatus::Ok,
                Some(missing),
                "partial: symbols missing for SSFIV.exe/DEF1/SSFIV.exe.sym",
            ),
            (WalkStatus::WalkFailed, None, "walk_failed"),
            (
                WalkStatus::WalkFailed,
                Some(both),
                "walk_failed: symbols unreadable for Launcher.pdb/ABC1/Launcher.sym; symbols missing for SSFIV.exe/DEF1/SSFIV.exe.sym",
            ),
            (WalkStatus::InvalidDump, None, "invalid_dump"),
            (WalkStatus::InputBudget, None, "input_budget"),
            (WalkStatus::Timeout, None, "timeout"),
            (WalkStatus::WorkerFailed, None, "worker_failed"),
        ]
    }
    fn frames() -> (Vec<Frame>, Vec<Thread>) {
        let frame = |i: u64| Frame {
            instruction: 0x401000 + i,
            module: "Launcher.exe".into(),
            offset: 0x1000 + i,
            function: Some(format!("frame_{i}")),
            filename: Some("launcher.cpp".into()),
            line: Some(42),
        };
        (
            vec![frame(0), frame(1)],
            vec![Thread {
                id: 9,
                frames: vec![frame(2)],
            }],
        )
    }
    fn assert_frames(crash: &[Frame], threads: &[Thread]) {
        assert_eq!(crash.len(), 2);
        assert_eq!(crash[1].function.as_deref(), Some("frame_1"));
        assert_eq!(crash[1].offset, 0x1001);
        assert_eq!(threads[0].id, 9);
        assert_eq!(threads[0].frames[0].instruction, 0x401002);
    }

    #[test]
    fn new_worker_output_keeps_the_legacy_status_for_an_older_parent() {
        for (status, problems, text) in compatibility_cases() {
            let (crash_frames, threads) = frames();
            let walk = Walk {
                code: Some(0xc0000005),
                crashing_thread: Some(7),
                crash_frames,
                threads,
                status,
                symbol_problems: problems.map(str::to_owned),
                unreadable_symbols: vec!["Launcher.pdb/ABC1/Launcher.sym".into()],
                ..Walk::default()
            };
            let json = serde_json::to_value(&walk).unwrap();
            assert_eq!(json["status"], text);
            assert!(json.get("symbol_problems").is_none());
            let old: LegacyWalk = serde_json::from_value(json).unwrap();
            assert_eq!(old.status, text);
            assert_eq!(old.code, Some(0xc0000005));
            assert_eq!(old.crashing_thread, Some(7));
            assert_eq!(old.unreadable_symbols, ["Launcher.pdb/ABC1/Launcher.sym"]);
            assert_frames(&old.crash_frames, &old.threads);
        }
    }

    #[test]
    fn old_worker_output_decodes_into_the_typed_status() {
        for (status, problems, text) in compatibility_cases() {
            let (crash_frames, threads) = frames();
            let old = LegacyWalk {
                crash_frames,
                threads,
                status: text.into(),
                ..LegacyWalk::default()
            };
            let walk: Walk = serde_json::from_slice(&serde_json::to_vec(&old).unwrap()).unwrap();
            assert_eq!(walk.status, status);
            assert_eq!(walk.symbol_problems.as_deref(), problems);
            assert_eq!(walk.symbolication(), text);
            assert_frames(&walk.crash_frames, &walk.threads);
        }
        let unknown = LegacyWalk {
            status: "unknown_status".into(),
            ..LegacyWalk::default()
        };
        assert!(serde_json::from_value::<Walk>(serde_json::to_value(unknown).unwrap()).is_err());
    }

    fn modules(padded: bool, count: usize, name_bytes: usize, cv_bytes: usize) -> Vec<u8> {
        let offset = 44;
        let header = if padded { 8 } else { 4 };
        let size = header + count * 108;
        let name = offset + size;
        let cv = name + 4 + name_bytes;
        let mut bytes = vec![0; cv + cv_bytes];
        bytes[..4].copy_from_slice(b"MDMP");
        let put = |bytes: &mut [u8], at: usize, value: usize| {
            bytes[at..at + 4].copy_from_slice(&(value as u32).to_le_bytes());
        };
        put(&mut bytes, 8, 1);
        put(&mut bytes, 12, 32);
        put(&mut bytes, 32, 4);
        put(&mut bytes, 36, size);
        put(&mut bytes, 40, offset);
        put(&mut bytes, offset, count);
        put(&mut bytes, name, name_bytes);
        for i in 0..count {
            let record = offset + header + i * 108;
            put(&mut bytes, record + 20, name);
            put(&mut bytes, record + 76, cv_bytes);
            put(&mut bytes, record + 80, cv);
        }
        bytes
    }

    #[test]
    fn padded_module_records_are_rejected_before_parser() {
        assert!(bounded_dump(modules(false, 1, 16, 16)).is_some());
        assert!(bounded_dump(modules(true, 1, 16, 16)).is_none());
        // The original bypass: all 256 padded records share a 2 MiB name.
        assert!(bounded_dump(modules(true, 256, 2 * 1024 * 1024, 0)).is_none());
    }

    #[test]
    fn shared_oversized_names_and_codeview_records_are_rejected() {
        assert!(bounded_dump(modules(false, 256, 2 * 1024 * 1024, 0)).is_none());
        for padded in [false, true] {
            assert!(bounded_dump(modules(padded, 1, 16, 1025)).is_none());
        }
        assert!(bounded_dump(modules(false, 1, 1024, 1024)).is_some());
        assert!(bounded_dump(modules(false, 1, 15, 0)).is_none());
    }

    #[test]
    fn module_name_and_codeview_referenced_ranges_are_checked() {
        let mut name = modules(false, 1, 16, 0);
        name.pop();
        assert!(bounded_dump(name).is_none());
        let mut cv = modules(false, 1, 16, 16);
        cv.pop();
        assert!(bounded_dump(cv).is_none());
        let mut rva = modules(false, 1, 16, 16);
        rva[44 + 4 + 80..44 + 4 + 84].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(bounded_dump(rva).is_none());
    }

    // One thread (id 7) whose stored stack is [0x800000, +16) and a range
    // [0x7fffe0, 0x800000) adjacent to it, with the context's esp as given.
    fn splice_fixture(esp: u32) -> (Vec<u8>, usize, usize) {
        let mut bytes = vec![0u8; 700];
        let put = |bytes: &mut Vec<u8>, at: usize, value: u32| {
            bytes[at..at + 4].copy_from_slice(&value.to_le_bytes());
        };
        put(&mut bytes, 0, 1);
        put(&mut bytes, 4, 7); // record: thread id
        bytes[28..36].copy_from_slice(&0x800000u64.to_le_bytes()); // stack start
        put(&mut bytes, 36, 16); // stack size
        put(&mut bytes, 40, 400); // stack rva
        put(&mut bytes, 44, 200); // context size
        put(&mut bytes, 48, 100); // context rva
        put(&mut bytes, 100 + 0xC4, esp);
        let stream = 500;
        for (i, v) in [1, 1, 7, 0x7fffe0, 32].into_iter().enumerate() {
            put(&mut bytes, stream + i * 4, v);
        }
        (bytes, stream, 20 + 32)
    }

    #[test]
    fn a_range_is_joined_only_while_the_context_esp_is_in_it() {
        let (bytes, stream, size) = splice_fixture(0x7fffe8);
        assert_eq!(plan_stack_splice(&bytes, 0, stream, size).unwrap().len(), 1);
        // The context moved below the range since it was read: record stays.
        let (bytes, stream, size) = splice_fixture(0x7fffd0);
        assert!(
            plan_stack_splice(&bytes, 0, stream, size)
                .unwrap()
                .is_empty()
        );
        // ... or past the stored stack.
        let (bytes, stream, size) = splice_fixture(0x800010);
        assert!(
            plan_stack_splice(&bytes, 0, stream, size)
                .unwrap()
                .is_empty()
        );
    }
}
