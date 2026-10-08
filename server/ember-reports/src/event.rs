use crate::{
    intake::{Kind, Report, hex},
    symbolicate::{Frame, Walk, exception_name},
};
use serde_json::{Value, json};
use std::time::{SystemTime, UNIX_EPOCH};

pub const EVENT_BYTES: usize = 900 * 1024;

fn frame(f: &Frame) -> Value {
    let in_app = ["launcher.exe", "sidecar.dll", "updater.exe", "sf4-net.exe"]
        .contains(&f.module.to_ascii_lowercase().as_str());
    let mut value = json!({
        "function": f.function.clone().unwrap_or_else(|| format!("{}+0x{:x}", f.module, f.offset)),
        "package": f.module,
        "instruction_addr": format!("0x{:x}", f.instruction),
        "in_app": in_app,
    });
    if let Some(filename) = &f.filename {
        value["filename"] = json!(filename);
    }
    if let Some(line) = f.line {
        value["lineno"] = json!(line);
    }
    value
}
fn frames(frames: &[Frame]) -> Vec<Value> {
    // rust-minidump is innermost-first; Sentry is innermost-last. Keep the
    // closest 64 frames before reversing, not the oldest 64 callers.
    frames.iter().take(64).rev().map(frame).collect()
}
fn size(value: &Value) -> usize {
    serde_json::to_vec(value)
        .expect("JSON values serialize")
        .len()
}
pub fn tail(text: &str, bytes: usize) -> &str {
    let mut start = text.len().saturating_sub(bytes);
    while !text.is_char_boundary(start) {
        start += 1;
    }
    &text[start..]
}

pub fn build(id: &str, report: &Report, walk: &Walk, dump_path: Option<&str>) -> Vec<u8> {
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
    let mut event = json!({
        "event_id": id,
        "timestamp": SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs_f64(),
        "platform": "native",
        "level": if m.kind == Kind::Crash { "error" } else { "warning" },
        "release": m.app_version,
        "environment": m.channel,
        "tags": { "channel": m.channel, "build_id": &m.build_id[..12], "kind": m.kind, "windows_version": m.windows_version },
        "extra": { "report_id": id, "comment": m.comment, "logs": {}, "source_revision": m.source_revision,
            "build_id": m.build_id, "exit_code": m.exit_code, "exception_code": code.map(|c| format!("0x{c:08x}")),
            "crash_address": address.map(|a| format!("0x{a:x}")), "symbolication": walk.status },
    });
    if let Some(code) = code {
        event["tags"]["exception_code"] = json!(format!("0x{code:08x}"));
    }
    if let Some(path) = dump_path {
        event["extra"]["minidump_path"] = json!(path);
    }
    if m.kind == Kind::Crash {
        let mut crash_frames = walk.crash_frames.clone();
        if crash_frames.is_empty()
            && let Some(instruction) = m.crash_address.as_deref().and_then(|s| hex(s, 16))
        {
            crash_frames.push(Frame {
                instruction,
                offset: instruction,
                module: "unknown".into(),
                ..Frame::default()
            });
        }
        event["exception"] = json!({ "values": [{
            "type": reason,
            "value": address.map_or_else(|| reason.clone(), |a| format!("{reason} at 0x{a:x}")),
            "thread_id": walk.crashing_thread,
            "mechanism": { "type": "minidump", "handled": false },
            "stacktrace": { "frames": frames(&crash_frames) },
        }] });
        if !crash_frames.iter().any(|f| f.function.is_some())
            && let Some(top) = crash_frames.first()
        {
            event["fingerprint"] = json!([
                "{{ default }}",
                format!("{}+0x{:x}", top.module, top.offset)
            ]);
        }
    } else {
        event["message"] = json!(m.comment.as_deref().unwrap_or(""));
    }
    event["threads"] = json!({ "values": [] });
    let mut truncated = false;
    for thread in &walk.threads {
        let value = json!({ "id": thread.id, "crashed": false, "current": false,
            "stacktrace": { "frames": frames(&thread.frames) } });
        event["threads"]["values"]
            .as_array_mut()
            .unwrap()
            .push(value);
        // Reserve room for logs and the truncation marker. The crashing thread
        // always takes precedence over auxiliary threads.
        if size(&event) >= EVENT_BYTES - 64 * 1024 {
            event["threads"]["values"].as_array_mut().unwrap().pop();
            truncated = true;
            break;
        }
    }
    event["extra"]["threads_truncated"] = json!(truncated);
    let mut remaining = report.logs.len();
    for (name, text) in &report.logs {
        let budget = (EVENT_BYTES - 1).saturating_sub(size(&event) + name.len() + 8) / remaining;
        remaining -= 1;
        // JSON escaping can expand a byte to six bytes. Fit serialized strings,
        // not raw bytes; binary search also preserves Unicode tail boundaries.
        let (mut low, mut high) = (0, text.len());
        while low < high {
            let mid = low + (high - low).div_ceil(2);
            let encoded = serde_json::to_vec(tail(text, mid)).unwrap();
            if encoded.len() <= budget {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        event["extra"]["logs"][name] = json!(tail(text, low));
    }
    let encoded = serde_json::to_vec(&event).unwrap();
    assert!(
        encoded.len() < EVENT_BYTES,
        "bounded event exceeded its budget"
    );
    encoded
}
