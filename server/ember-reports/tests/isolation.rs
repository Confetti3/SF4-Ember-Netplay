#![cfg(target_os = "linux")]
mod support;
use ember_reports::{
    limits::Gate,
    symbolicate::{FRAMES, THREADS},
    worker::Worker,
};
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};
use support::Temp;

// A compact shared x86 stack whose entire frame-pointer chain has return
// addresses outside known modules. This path never consults the symbol provider.
fn frame_pointer_chain(frames: u32, threads: u32) -> Vec<u8> {
    use minidump_synth::{
        DumpString, Exception, Memory, Module, SynthMinidump, SystemInfo, Thread, x86_context,
    };
    use test_assembler::{Endian, Section};
    let e = Endian::Little;
    let base = 0x800000_u32;
    let ip = 0x900000_u32;
    let mut stack = Section::with_endian(e);
    for i in 0..frames {
        stack = stack.D32(base + (i + 1) * 8).D32(ip);
    }
    let stack = Memory::with_section(stack, u64::from(base));
    let context = x86_context(e, ip, base);
    let name = DumpString::new("known.exe", e);
    let module = Module::new(e, 0x400000, 0x10000, &name, 0, 0, None);
    let mut dump = SynthMinidump::with_endian(e)
        .add(name)
        .add_module(module)
        .add_system_info(SystemInfo::new(e).set_platform_id(2));
    for id in 0..threads {
        dump = dump.add_thread(Thread::new(e, id, &stack, &context));
    }
    let mut exception = Exception::new(e);
    exception.thread_id = 0;
    exception.exception_record.exception_code = 0xc0000005;
    exception.exception_record.exception_address = u64::from(ip);
    exception.thread_context = (0, 0);
    let mut bytes = dump
        .add(context)
        .add_memory(stack)
        .add_exception(exception)
        .finish()
        .unwrap();
    let get =
        |bytes: &[u8], p: usize| u32::from_le_bytes(bytes[p..p + 4].try_into().unwrap()) as usize;
    let count = get(&bytes, 8);
    let directory = get(&bytes, 12);
    let record = (0..count)
        .map(|i| directory + i * 12)
        .find(|p| get(&bytes, *p) == 3)
        .unwrap();
    let thread_list = get(&bytes, record + 8);
    let context_rva = get(&bytes, thread_list + 4 + 44);
    // CONTEXT_X86.ebp: x86_context starts it at zero; seed the real chain.
    bytes[context_rva + 180..context_rva + 184].copy_from_slice(&base.to_le_bytes());
    assert!(bytes.len() < ember_reports::intake::DUMP_BYTES);
    bytes
}

#[tokio::test]
async fn long_unknown_module_chain_is_isolated_and_worker_slot_is_reusable() {
    let temp = Temp::new();
    let worker = Worker::new(PathBuf::from(env!("CARGO_BIN_EXE_ember-reports")));
    // Confirm the fixture really walks through more than the export limit.
    let short = worker
        .walk(
            frame_pointer_chain(256, 1),
            temp.0.clone(),
            Duration::from_secs(5),
        )
        .await;
    assert_eq!(short.status, "ok");
    assert_eq!(short.crash_frames.len(), FRAMES);
    assert!(short.crash_frames.iter().all(|f| f.module == "unknown"));
    let gate = Gate::new(1, 0);
    let ticket = gate
        .start(gate.reserve().unwrap(), Duration::from_secs(1))
        .await
        .unwrap();
    let bytes = frame_pointer_chain(300_000, THREADS as u32);
    let started = Instant::now();
    let walk = worker
        .walk(bytes, temp.0.clone(), Duration::from_secs(1))
        .await;
    assert!(
        matches!(walk.status.as_str(), "timeout" | "worker_failed"),
        "{}",
        walk.status
    );
    assert!(started.elapsed() < Duration::from_secs(5));
    assert!(gate.reserve().is_err());
    drop(ticket); // worker.walk returns only after the process is reaped.
    assert!(gate.reserve().is_ok());
    let healthy = worker
        .walk(
            frame_pointer_chain(256, 1),
            temp.0.clone(),
            Duration::from_secs(5),
        )
        .await;
    assert_eq!(healthy.status, "ok");
}

#[tokio::test]
async fn disconnected_request_keeps_admission_until_worker_is_reaped() {
    use axum::{body::Body, extract::ConnectInfo, http::Request};
    use ember_reports::{
        App,
        config::{Config, Limits},
        router,
    };
    use std::{net::SocketAddr, os::unix::fs::PermissionsExt};
    use tower::ServiceExt;

    let temp = Temp::new();
    let executable = temp.0.join("fake-worker");
    // The supervisor's real command/protocol, with a worker that never yields.
    std::fs::write(
        &executable,
        "#!/bin/sh\necho $$ > \"$2/worker.pid\"\nwhile :; do :; done\n",
    )
    .unwrap();
    std::fs::set_permissions(&executable, std::fs::Permissions::from_mode(0o700)).unwrap();
    let app = App::with_worker(
        Config {
            state_dir: temp.0.clone(),
            limits: Limits {
                workers: 1,
                queue: 0,
                processing_secs: 1,
                ..Limits::default()
            },
            ..Config::default()
        },
        "publictestkey",
        Worker::new(executable),
    )
    .await
    .unwrap();
    let meta = serde_json::json!({ "schema": 1, "kind": "problem", "app_version": "test", "channel": "beta", "build_id": "a".repeat(64), "source_revision": "test", "windows_version": "test" });
    let body = format!(
        "--test\r\nContent-Disposition: form-data; name=\"meta\"\r\n\r\n{meta}\r\n--test\r\nContent-Disposition: form-data; name=\"minidump\"\r\n\r\nMDMP\r\n--test--\r\n"
    );
    let mut request = Request::builder()
        .method("POST")
        .uri("/report/v1")
        .header("content-type", "multipart/form-data; boundary=test")
        .body(Body::from(body))
        .unwrap();
    request
        .extensions_mut()
        .insert(ConnectInfo("127.0.0.1:1234".parse::<SocketAddr>().unwrap()));
    let task = tokio::spawn(router(app.clone()).oneshot(request));
    let pid_file = temp.0.join("symbols/worker.pid");
    let pid = tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if let Some(pid) = std::fs::read_to_string(&pid_file)
                .ok()
                .and_then(|s| s.trim().parse::<u32>().ok())
            {
                break pid;
            }
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    })
    .await
    .unwrap();
    task.abort();
    let _ = task.await;
    assert!(app.gate.reserve().is_err());
    tokio::time::timeout(Duration::from_secs(5), async {
        while app.store.pending().await.unwrap().is_empty() {
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .unwrap();
    assert!(!std::path::Path::new(&format!("/proc/{pid}")).exists());
    assert!(app.gate.reserve().is_ok());
}
