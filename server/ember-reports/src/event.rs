use crate::{
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
    logs: BTreeMap<&'a str, &'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    minidump_path: Option<&'a str>,
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
    kind: Kind,
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

pub fn build(id: &ReportId, report: &Report, walk: &Walk, dump_path: Option<&str>) -> Vec<u8> {
    let m = &report.meta;
    let code = walk.code.or_else(|| {
        m.exception_code
            .as_deref()
            .and_then(|s| hex(s, 8))
            .map(|v| v as u32)
    });
    let address = walk
        .address
        .or_else(|| m.crash_address.as_deref().and_then(|s| hex(s, 16)));
    let reason = walk
        .reason
        .clone()
        .or_else(|| code.map(|code| exception_name(code, None)))
        .unwrap_or_else(|| "Native crash".into());
    let fallback;
    let crash_frames: &[Frame] = if walk.crash_frames.is_empty()
        && let Some(instruction) = m.crash_address.as_deref().and_then(|s| hex(s, 16))
    {
        fallback = [Frame {
            instruction,
            offset: instruction,
            module: "unknown".into(),
            ..Frame::default()
        }];
        &fallback
    } else {
        &walk.crash_frames
    };
    let crash = m.kind == Kind::Crash;
    let mut event = Event {
        environment: m.channel,
        event_id: id.as_str(),
        exception: crash.then(|| Values {
            values: vec![Exception {
                mechanism: Mechanism {
                    handled: false,
                    kind: "minidump",
                },
                stacktrace: stacktrace(crash_frames),
                thread_id: walk.crashing_thread,
                kind: &reason,
                value: address.map_or_else(|| reason.clone(), |a| format!("{reason} at 0x{a:x}")),
            }],
        }),
        extra: Extra {
            build_id: &m.build_id,
            comment: m.comment.as_deref(),
            crash_address: address.map(|a| format!("0x{a:x}")),
            exception_code: code.map(|c| format!("0x{c:08x}")),
            exit_code: m.exit_code,
            logs: BTreeMap::new(),
            minidump_path: dump_path,
            report_id: id.as_str(),
            source_revision: &m.source_revision,
            symbolication: walk.symbolication(),
            threads_truncated: None,
        },
        fingerprint: if crash && !crash_frames.iter().any(|f| f.function.is_some()) {
            crash_frames.first().map(|top| {
                [
                    "{{ default }}".into(),
                    format!("{}+0x{:x}", top.module, top.offset),
                ]
            })
        } else {
            None
        },
        level: if crash { "error" } else { "warning" },
        message: (!crash).then(|| m.comment.as_deref().unwrap_or("")),
        platform: "native",
        release: &m.app_version,
        tags: Tags {
            build_id: &m.build_id[..12],
            channel: m.channel,
            exception_code: code.map(|c| format!("0x{c:08x}")),
            kind: m.kind,
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
