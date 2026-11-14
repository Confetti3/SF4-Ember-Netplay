use crate::{
    header::{self, Header},
    intake::{Channel, Kind, Report, hex},
    store::ReportId,
    symbolicate::{Frame, Walk, exception_name},
};
use serde::Serialize;
use std::{
    borrow::Cow,
    collections::BTreeMap,
    io,
    time::{SystemTime, UNIX_EPOCH},
};

pub const EVENT_BYTES: usize = 900 * 1024;

// Sentry event fields. Each struct declares its keys in sorted order, the
// order serde_json writes for maps, so the stored JSON keeps one layout.
#[derive(Serialize)]
struct Event<'a> {
    environment: Channel,
    event_id: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    exception: Option<Values<Exception<'a>>>,
    extra: Extra<'a>,
    #[serde(skip_serializing_if = "Option::is_none")]
    fingerprint: Option<[String; 2]>,
    level: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    message: Option<&'a str>,
    platform: &'static str,
    release: &'a str,
    tags: Tags<'a>,
    threads: Values<Thread<'a>>,
    timestamp: f64,
}
#[derive(Serialize)]
struct Values<T> {
    values: Vec<T>,
}
#[derive(Serialize)]
struct Extra<'a> {
    build_id: &'a str,
    comment: Option<&'a str>,
    crash_address: Option<String>,
    exception_code: Option<String>,
    exit_code: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    game_log: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    hint: Option<&'a str>,
    logs: BTreeMap<&'a str, &'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    minidump_path: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    phase: Option<&'a str>,
    report_id: &'a str,
    source_revision: &'a str,
    symbolication: String,
    // Unset while thread sizes are counted against the thread budget.
    #[serde(skip_serializing_if = "Option::is_none")]
    threads_truncated: Option<bool>,
}
#[derive(Serialize)]
struct Tags<'a> {
    build_id: &'a str,
    channel: Channel,
    #[serde(skip_serializing_if = "Option::is_none")]
    exception_code: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    exit_code: Option<String>,
    kind: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    phase: Option<&'a str>,
    windows_version: &'a str,
}
#[derive(Serialize)]
struct Exception<'a> {
    mechanism: Mechanism,
    stacktrace: Stacktrace<'a>,
    thread_id: Option<u32>,
    #[serde(rename = "type")]
    kind: &'a str,
    value: String,
}
#[derive(Serialize)]
struct Mechanism {
    handled: bool,
    #[serde(rename = "type")]
    kind: &'static str,
}
#[derive(Serialize)]
struct Thread<'a> {
    crashed: bool,
    current: bool,
    id: u32,
    stacktrace: Stacktrace<'a>,
}
#[derive(Serialize)]
struct Stacktrace<'a> {
    frames: Vec<StackFrame<'a>>,
}
#[derive(Serialize)]
struct StackFrame<'a> {
    #[serde(skip_serializing_if = "Option::is_none")]
    filename: Option<&'a str>,
    function: Cow<'a, str>,
    in_app: bool,
    instruction_addr: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    lineno: Option<u32>,
    package: &'a str,
}

fn frame(f: &Frame) -> StackFrame<'_> {
    StackFrame {
        filename: f.filename.as_deref(),
        function: f.function.as_deref().map_or_else(
            || Cow::Owned(format!("{}+0x{:x}", f.module, f.offset)),
            Cow::Borrowed,
        ),
        in_app: ["launcher.exe", "sidecar.dll", "updater.exe", "sf4-net.exe"]
            .contains(&f.module.to_ascii_lowercase().as_str()),
        instruction_addr: format!("0x{:x}", f.instruction),
        lineno: f.line,
        package: &f.module,
    }
}
fn stacktrace(frames: &[Frame]) -> Stacktrace<'_> {
    // rust-minidump is innermost-first; Sentry is innermost-last. Keep the
    // closest 64 frames before reversing, not the oldest 64 callers.
    Stacktrace {
        frames: frames.iter().take(64).rev().map(frame).collect(),
    }
}
struct Counter(usize);
impl io::Write for Counter {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        self.0 += bytes.len();
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
fn size<T: Serialize + ?Sized>(value: &T) -> usize {
    let mut counter = Counter(0);
    serde_json::to_writer(&mut counter, value).expect("event values serialize");
    counter.0
}
pub fn tail(text: &str, bytes: usize) -> &str {
    let mut start = text.len().saturating_sub(bytes);
    while !text.is_char_boundary(start) {
        start += 1;
    }
    &text[start..]
}

// Without a dump the stack is only the crash address, which moves with each
// load of a DLL, so one fault opened a new issue per run. The game's crash
// log names it as `address=0x... module=Name+0x...`. Only the newest line for
// this report's address counts: the log keeps earlier runs, and when that line
// names no module an older crash at the same address must not stand in for it.
fn logged_module(report: &Report, address: u64) -> Option<(String, u64)> {
    let log = report.logs.get("sf4e-crash.log")?;
    let logged_address = |line: &str| {
        let (_, rest) = line.split_once("address=")?;
        hex(rest.split(' ').next()?, 16)
    };
    let line = log
        .lines()
        .rev()
        .find(|line| logged_address(line) == Some(address))?;
    let (_, rest) = line.split_once("module=")?;
    let (name, offset) = rest.split_whitespace().next()?.rsplit_once("+0x")?;
    let named = !name.is_empty()
        && name.len() <= 64
        && name
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b));
    if !named {
        return None;
    }
    Some((name.to_owned(), hex(offset, 8)?))
}

// What a report without a stack or an exception code says happened, from its
// header, exit code and phase, so that start failures, frozen games and other
// exits each open an issue of their own instead of one "Native crash".
struct Outcome {
    title: String,
    value: String,
    // The fingerprint's second part.
    print: String,
}
fn outcome(m: &Meta, header: &Header) -> Option<Outcome> {
    let exit = m.exit_code.map(|n| n as u32);
    if header.start_failure {
        let exit_text = exit.map(|c| format!("0x{c:X}"));
        let phase = header.phase.as_deref();
        let when = match phase {
            Some("before_signal") => " before the start-up signal",
            Some("after_signal") => " after the start-up signal",
            _ => "",
        };
        let how = exit_text
            .as_deref()
            .map_or_else(|| "no exit code".to_owned(), |c| format!("exit {c}"));
        return Some(Outcome {
            title: "Start failure".into(),
            value: format!("{how}{when}"),
            print: format!(
                "start_failure:{}:{}",
                exit_text.as_deref().unwrap_or("none").to_ascii_lowercase(),
                phase.unwrap_or("unknown")
            ),
        });
    }
    if m.kind != Kind::Crash {
        return None;
    }
    let code = exit?;
    let title = if code == 0xCFFF_FFFF {
        "Game frozen".to_owned()
    } else {
        format!("Game exit 0x{code:X}")
    };
    Some(Outcome {
        value: format!("exit 0x{code:X}"),
        print: format!("exit:0x{code:x}"),
        title,
    })
}

pub fn build(id: &ReportId, report: &Report, walk: &Walk, dump_path: Option<&str>) -> Vec<u8> {
    let m = &report.meta;
    // Only a crash report's header is read; a typed report is its comment.
    let header = if m.kind == Kind::Crash {
        header::parse(report)
    } else {
        Header::default()
    };
    let code = walk.code.or_else(|| {
        m.exception_code
            .as_deref()
            .and_then(|s| hex(s, 8))
            .map(|v| v as u32)
    });
    let address = walk
        .address
        .or_else(|| m.crash_address.as_deref().and_then(|s| hex(s, 16)));
    let what = outcome(m, &header);
    // A start failure is named by what happened, never by a stack or a code a
    // record happened to leave. Other reports are named by their stack's
    // reason or exception code, and only without either by their exit.
    let start = header.start_failure;
    let named_by_outcome = start || (walk.reason.is_none() && code.is_none());
    let reason = match &what {
        Some(o) if named_by_outcome => o.title.clone(),
        _ => walk
            .reason
            .clone()
            .or_else(|| code.map(|code| exception_name(code, None)))
            .unwrap_or_else(|| "Native crash".into()),
    };
    let fallback;
    let crash_frames: &[Frame] = if walk.crash_frames.is_empty()
        && let Some(instruction) = m.crash_address.as_deref().and_then(|s| hex(s, 16))
    {
        let (module, offset) =
            logged_module(report, instruction).unwrap_or_else(|| ("unknown".into(), instruction));
        fallback = [Frame {
            instruction,
            offset,
            module,
            ..Frame::default()
        }];
        &fallback
    } else {
        &walk.crash_frames
    };
    let crash = m.kind == Kind::Crash;
    // Reports of a game that ended carry an exception, and so a title.
    let ended = m.kind != Kind::Problem;
    // The crash log's module and offset name the fault the same way on every
    // load of the DLL, where the raw address moves.
    let logged = walk
        .crash_frames
        .is_empty()
        .then(|| crash_frames.first())
        .flatten();
    let value = match (&what, logged) {
        (Some(o), _) if named_by_outcome => o.value.clone(),
        (_, Some(top)) if address.is_some() => {
            format!("{reason} at {}+0x{:x}", top.module, top.offset)
        }
        _ => address.map_or_else(|| reason.clone(), |a| format!("{reason} at 0x{a:x}")),
    };
    let mut event = Event {
        environment: m.channel,
        event_id: id.as_str(),
        exception: ended.then(|| Values {
            values: vec![Exception {
                mechanism: Mechanism {
                    handled: false,
                    kind: "minidump",
                },
                stacktrace: stacktrace(crash_frames),
                thread_id: walk.crashing_thread,
                kind: &reason,
                value,
            }],
        }),
        extra: Extra {
            build_id: &m.build_id,
            comment: m.comment.as_deref(),
            crash_address: address.map(|a| format!("0x{a:x}")),
            exception_code: code.map(|c| format!("0x{c:08x}")),
            exit_code: m.exit_code,
            game_log: header.game_log.as_deref(),
            hint: header.hint.as_deref(),
            logs: BTreeMap::new(),
            minidump_path: dump_path,
            phase: header.phase.as_deref(),
            report_id: id.as_str(),
            source_revision: &m.source_revision,
            symbolication: walk.symbolication(),
            threads_truncated: None,
        },
        fingerprint: if start {
            what.as_ref()
                .map(|o| ["{{ default }}".into(), o.print.clone()])
        } else if crash && !crash_frames.iter().any(|f| f.function.is_some()) {
            crash_frames
                .first()
                .map(|top| format!("{}+0x{:x}", top.module, top.offset))
                .or_else(|| {
                    what.as_ref()
                        .filter(|_| named_by_outcome)
                        .map(|o| o.print.clone())
                })
                .map(|print| ["{{ default }}".into(), print])
        } else {
            None
        },
        level: if crash && !start { "error" } else { "warning" },
        message: (m.kind == Kind::Problem).then(|| m.comment.as_deref().unwrap_or("")),
        platform: "native",
        release: &m.app_version,
        tags: Tags {
            build_id: &m.build_id[..12],
            channel: m.channel,
            exception_code: code.map(|c| format!("0x{c:08x}")),
            exit_code: m.exit_code.map(|n| format!("0x{:08x}", n as u32)),
            kind: if start {
                "start_failure"
            } else if crash {
                "crash"
            } else {
                "problem"
            },
            phase: header.phase.as_deref(),
            windows_version: &m.windows_version,
        },
        threads: Values { values: Vec::new() },
        timestamp: SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs_f64(),
    };
    // Count each thread once as it is added, rather than reserialising the
    // growing event. Reserve room for logs and the truncation marker. The
    // crashing thread always takes precedence over auxiliary threads.
    let mut used = size(&event);
    let mut truncated = false;
    for thread in &walk.threads {
        let value = Thread {
            crashed: false,
            current: false,
            id: thread.id,
            stacktrace: stacktrace(&thread.frames),
        };
        let added = size(&value) + usize::from(!event.threads.values.is_empty());
        if used + added >= EVENT_BYTES - 64 * 1024 {
            truncated = true;
            break;
        }
        used += added;
        event.threads.values.push(value);
    }
    event.extra.threads_truncated = Some(truncated);
    let mut remaining = report.logs.len();
    for (name, text) in &report.logs {
        let budget = (EVENT_BYTES - 1).saturating_sub(size(&event) + name.len() + 8) / remaining;
        remaining -= 1;
        // JSON escaping can expand a byte to six bytes. Fit serialized strings,
        // not raw bytes; binary search also preserves Unicode tail boundaries.
        let (mut low, mut high) = (0, text.len());
        while low < high {
            let mid = low + (high - low).div_ceil(2);
            if size(tail(text, mid)) <= budget {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        event.extra.logs.insert(name, tail(text, low));
    }
    let encoded = serde_json::to_vec(&event).expect("event values serialize");
    assert!(
        encoded.len() < EVENT_BYTES,
        "bounded event exceeded its budget"
    );
    encoded
}
