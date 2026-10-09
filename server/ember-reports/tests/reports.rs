use axum::{
    Router,
    body::{Body, to_bytes},
    extract::{ConnectInfo, State},
    http::{Request, StatusCode},
    routing::post,
};
use ember_reports::{
    App,
    bugsink::{Bugsink, Delivery},
    config::{Config, Limits},
    event::{self, EVENT_BYTES},
    intake::{self, Kind, Report},
    limits::{Gate, RateLimiter, client_address},
    router,
    store::Store,
    symbolicate::{Frame, Thread, Walk},
    worker::Worker,
};
use serde_json::{Value, json};
use std::{
    collections::BTreeMap,
    future::IntoFuture,
    net::{IpAddr, SocketAddr},
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    time::{Duration, Instant, SystemTime},
};
use tower::ServiceExt;

struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        let path =
            std::env::temp_dir().join(format!("ember-reports-test-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        assert_eq!(self.0.parent(), Some(std::env::temp_dir().as_path()));
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn meta() -> Value {
    json!({ "schema": 1, "kind": "crash", "app_version": "0.8.0", "channel": "beta", "build_id": "a".repeat(64),
        "source_revision": "0123456789abcdef", "windows_version": "Windows 11 24H2", "exception_code": "0xc0000005",
        "crash_address": "0x401234", "comment": "It stopped after joining." })
}
fn report() -> Report {
    Report {
        meta: serde_json::from_value(meta()).unwrap(),
        logs: BTreeMap::new(),
        minidump: None,
    }
}
fn parts(parts: Vec<(&str, Option<&str>, Vec<u8>)>) -> Vec<u8> {
    let mut bytes = Vec::new();
    for (name, filename, contents) in parts {
        bytes.extend_from_slice(
            format!("--ember-test\r\nContent-Disposition: form-data; name=\"{name}\"").as_bytes(),
        );
        if let Some(filename) = filename {
            bytes.extend_from_slice(format!("; filename=\"{filename}\"").as_bytes());
        }
        bytes.extend_from_slice(b"\r\n\r\n");
        bytes.extend_from_slice(&contents);
        bytes.extend_from_slice(b"\r\n");
    }
    bytes.extend_from_slice(b"--ember-test--\r\n");
    bytes
}
fn request(body: Body) -> Request<Body> {
    let mut request = Request::builder()
        .method("POST")
        .uri("/report/v1")
        .header("content-type", "multipart/form-data; boundary=ember-test")
        .body(body)
        .unwrap();
    request.extensions_mut().insert(ConnectInfo(
        "127.0.0.1:54321".parse::<SocketAddr>().unwrap(),
    ));
    request
}
fn worker() -> Worker {
    Worker::new(PathBuf::from(env!("CARGO_BIN_EXE_ember-reports")))
}
async fn app(temp: &Temp) -> Arc<App> {
    App::with_worker(
        Config {
            state_dir: temp.0.clone(),
            ..Config::default()
        },
        "publictestkey",
        worker(),
    )
    .await
    .unwrap()
}
async fn validate(bytes: Vec<u8>) -> StatusCode {
    let temp = Temp::new();
    router(app(&temp).await)
        .oneshot(request(Body::from(bytes)))
        .await
        .unwrap()
        .status()
}
fn meta_part() -> (&'static str, Option<&'static str>, Vec<u8>) {
    ("meta", None, serde_json::to_vec(&meta()).unwrap())
}

#[tokio::test]
async fn multipart_accepts_all_allowlisted_logs_and_optional_dump() {
    let temp = Temp::new();
    let state = app(&temp).await;
    let mut input = vec![meta_part()];
    for name in intake::LOG_NAMES {
        input.push(("log", Some(name), "hello\n世界".as_bytes().to_vec()));
    }
    input.push(("minidump", Some("crash.dmp"), b"MDMPbroken".to_vec()));
    let response = router(state.clone())
        .oneshot(request(Body::from(parts(input))))
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::ACCEPTED);
    let value: Value =
        serde_json::from_slice(&to_bytes(response.into_body(), 1024).await.unwrap()).unwrap();
    let id = value["id"].as_str().unwrap();
    assert_eq!(id.len(), 32);
    let event: Value =
        serde_json::from_slice(&state.store.event(id).await.unwrap().unwrap()).unwrap();
    assert_eq!(event["extra"]["logs"].as_object().unwrap().len(), 4);
    assert_eq!(event["extra"]["report_id"], id);
    let dump = state
        .config
        .state_dir
        .join("dumps")
        .join(format!("{id}.dmp"));
    assert_eq!(std::fs::read(&dump).unwrap(), b"MDMPbroken");
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(
            std::fs::metadata(dump).unwrap().permissions().mode() & 0o777,
            0o600
        );
    }
}

#[tokio::test]
async fn exact_part_and_body_boundaries_are_accepted() {
    let mut dump = vec![0; intake::DUMP_BYTES];
    dump[..4].copy_from_slice(b"MDMP");
    let mut input = vec![meta_part(), ("minidump", None, dump)];
    for name in intake::LOG_NAMES {
        input.push(("log", Some(name), vec![b'x'; intake::LOG_BYTES]));
    }
    let mut bytes = parts(input);
    assert!(bytes.len() < intake::BODY_BYTES);
    // An epilogue is legal multipart data, and counts toward the HTTP cap.
    bytes.resize(intake::BODY_BYTES, b'x');
    assert_eq!(validate(bytes).await, StatusCode::ACCEPTED);
}

#[tokio::test]
async fn outbox_full_returns_503_and_leaves_accepted_event() {
    let temp = Temp::new();
    let config = Config {
        state_dir: temp.0.clone(),
        limits: Limits {
            outbox_count: 1,
            dump_count: 1,
            ..Limits::default()
        },
        ..Config::default()
    };
    let state = App::with_worker(config, "publictestkey", worker())
        .await
        .unwrap();
    let accepted = "a".repeat(32);
    state
        .store
        .reserve(&accepted, 2)
        .await
        .unwrap()
        .commit(b"{}", Some(b"MDMPaccepted"))
        .await
        .unwrap();
    let response = router(state.clone())
        .oneshot(request(Body::from(parts(vec![
            meta_part(),
            ("minidump", None, b"MDMP".to_vec()),
        ]))))
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::SERVICE_UNAVAILABLE);
    assert_eq!(state.store.event(&accepted).await.unwrap().unwrap(), b"{}");
    assert_eq!(std::fs::read_dir(temp.0.join("dumps")).unwrap().count(), 1);
    assert_eq!(
        std::fs::read(state.store.dump_path(&accepted)).unwrap(),
        b"MDMPaccepted"
    );
}

#[tokio::test]
async fn multipart_rejects_unknown_missing_duplicate_and_invalid_parts() {
    let bad = StatusCode::BAD_REQUEST;
    let cases = vec![
        (vec![], bad),
        (vec![("other", None, vec![])], bad),
        (vec![meta_part(), meta_part()], bad),
        (
            vec![(
                "meta",
                Some("meta.json"),
                serde_json::to_vec(&meta()).unwrap(),
            )],
            bad,
        ),
        (vec![("meta", None, b"{".to_vec())], bad),
        (vec![meta_part(), ("log", None, vec![])], bad),
        (vec![meta_part(), ("log", Some("secret.txt"), vec![])], bad),
        (vec![meta_part(), ("log", Some("../sf4e.log"), vec![])], bad),
        (
            vec![
                meta_part(),
                ("log", Some("sf4e.log"), vec![]),
                ("log", Some("sf4e.log"), vec![]),
            ],
            bad,
        ),
        (
            vec![meta_part(), ("log", Some("sf4e.log"), vec![0xff])],
            bad,
        ),
        (vec![meta_part(), ("minidump", None, vec![])], bad),
        (vec![meta_part(), ("minidump", None, b"WRNG".to_vec())], bad),
        (
            vec![
                meta_part(),
                ("minidump", None, b"MDMP".to_vec()),
                ("minidump", None, b"MDMP".to_vec()),
            ],
            bad,
        ),
        (
            vec![("meta", None, vec![b' '; intake::META_BYTES + 1])],
            StatusCode::PAYLOAD_TOO_LARGE,
        ),
        (
            vec![
                meta_part(),
                ("log", Some("sf4e.log"), vec![b'x'; intake::LOG_BYTES + 1]),
            ],
            StatusCode::PAYLOAD_TOO_LARGE,
        ),
        (
            vec![
                meta_part(),
                ("minidump", None, vec![0; intake::DUMP_BYTES + 1]),
            ],
            StatusCode::PAYLOAD_TOO_LARGE,
        ),
    ];
    for (input, status) in cases {
        assert_eq!(validate(parts(input)).await, status);
    }
    assert_eq!(validate(b"--wrong\r\n".to_vec()).await, bad);
    assert_eq!(
        validate(
            b"--ember-test\r\nContent-Disposition: form-data\r\n\r\nx\r\n--ember-test--\r\n"
                .to_vec()
        )
        .await,
        bad
    );
    let mut fifth = vec![meta_part()];
    for name in intake::LOG_NAMES {
        fifth.push(("log", Some(name), vec![]));
    }
    fifth.push(("log", Some("sf4e.log"), vec![]));
    assert_eq!(validate(parts(fifth)).await, bad);
}

#[tokio::test]
async fn metadata_rejections_and_unicode_comment_boundary() {
    let changes = [
        ("schema", json!(2)),
        ("schema", json!("1")),
        ("kind", json!("hang")),
        ("channel", json!("dev")),
        ("app_version", json!("")),
        ("source_revision", json!("")),
        ("windows_version", json!("x\n")),
        ("build_id", json!("a".repeat(63))),
        ("build_id", json!("g".repeat(64))),
        ("exception_code", json!("not-hex")),
        ("exception_code", json!(123)),
        ("crash_address", json!("0x10000000000000000")),
        ("exit_code", json!(4294967296_i64)),
        ("exit_code", json!(-2147483649_i64)),
        ("comment", json!("🦀".repeat(2001))),
        ("unexpected", json!(true)),
        ("app_version", json!("x".repeat(129))),
        ("source_revision", json!("x".repeat(129))),
        ("windows_version", json!("x".repeat(257))),
    ];
    for (key, value) in changes {
        let mut m = meta();
        m[key] = value;
        assert_eq!(
            validate(parts(vec![("meta", None, serde_json::to_vec(&m).unwrap())])).await,
            StatusCode::BAD_REQUEST,
            "{key}"
        );
    }
    for key in [
        "schema",
        "kind",
        "channel",
        "app_version",
        "source_revision",
        "windows_version",
        "build_id",
    ] {
        let mut m = meta();
        m.as_object_mut().unwrap().remove(key);
        assert_eq!(
            validate(parts(vec![("meta", None, serde_json::to_vec(&m).unwrap())])).await,
            StatusCode::BAD_REQUEST
        );
    }
    let mut m = meta();
    m["comment"] = json!("🦀".repeat(2000));
    assert_eq!(
        validate(parts(vec![("meta", None, serde_json::to_vec(&m).unwrap())])).await,
        StatusCode::ACCEPTED
    );
    let temp = Temp::new();
    let response = router(app(&temp).await)
        .oneshot(
            Request::builder()
                .method("POST")
                .uri("/report/v1")
                .body(Body::empty())
                .unwrap(),
        )
        .await
        .unwrap();
    // Missing transport peer fails closed rather than trusting a spoofed header.
    assert_eq!(response.status(), StatusCode::INTERNAL_SERVER_ERROR);
}

#[tokio::test]
async fn body_limit_covers_content_length_and_chunked_bodies() {
    let temp = Temp::new();
    let state = app(&temp).await;
    let mut declared = request(Body::empty());
    declared.headers_mut().insert(
        "content-length",
        (intake::BODY_BYTES + 1).to_string().parse().unwrap(),
    );
    assert_eq!(
        router(state.clone())
            .oneshot(declared)
            .await
            .unwrap()
            .status(),
        StatusCode::PAYLOAD_TOO_LARGE
    );
    // Unknown-size stream: valid parts followed by enough multipart epilogue
    // data to exceed the total limit, despite every individual part fitting.
    let mut body = parts(vec![meta_part()]);
    body.extend(std::iter::repeat_n(b'x', intake::BODY_BYTES));
    let chunks = body
        .chunks(32768)
        .map(|b| Ok::<_, std::io::Error>(axum::body::Bytes::copy_from_slice(b)))
        .collect::<Vec<_>>();
    let streamed = Body::from_stream(futures_util::stream::iter(chunks));
    assert_eq!(
        router(state)
            .oneshot(request(streamed))
            .await
            .unwrap()
            .status(),
        StatusCode::PAYLOAD_TOO_LARGE
    );
}

#[test]
fn rates_use_sliding_windows_and_bounded_address_table() {
    let t = Instant::now();
    let ip = "192.0.2.1".parse().unwrap();
    let rate = RateLimiter::new(Limits::default());
    for _ in 0..5 {
        assert!(rate.admit(ip, t).is_ok());
    }
    assert!(rate.admit(ip, t).unwrap_err() >= 600);
    assert!(rate.admit(ip, t + Duration::from_secs(599)).is_err());
    assert!(rate.admit(ip, t + Duration::from_secs(600)).is_ok());
    let rate = RateLimiter::new(Limits {
        tracked_addresses: 1,
        ..Limits::default()
    });
    assert!(rate.admit(ip, t).is_ok());
    assert!(rate.admit("192.0.2.2".parse().unwrap(), t).is_err());
    assert!(
        rate.admit("192.0.2.2".parse().unwrap(), t + Duration::from_secs(600))
            .is_ok()
    );
    let rate = RateLimiter::new(Limits::default());
    for i in 1..=300_u16 {
        assert!(
            rate.admit(
                IpAddr::V4(std::net::Ipv4Addr::new(
                    10,
                    0,
                    (i / 256) as u8,
                    (i % 256) as u8
                )),
                t
            )
            .is_ok()
        );
    }
    assert!(rate.admit(ip, t).unwrap_err() >= 3600);
    assert!(rate.admit(ip, t + Duration::from_secs(3600)).is_ok());
}

#[test]
fn only_exact_ipv4_loopback_peers_can_supply_real_ip() {
    let mut headers = axum::http::HeaderMap::new();
    headers.insert("x-real-ip", "192.0.2.1".parse().unwrap());
    assert_eq!(
        client_address("127.0.0.1".parse().unwrap(), &headers),
        "192.0.2.1".parse::<IpAddr>().unwrap()
    );
    for peer in ["::1", "127.0.0.2", "198.51.100.1"] {
        assert_eq!(
            client_address(peer.parse().unwrap(), &headers),
            peer.parse::<IpAddr>().unwrap()
        );
    }
    headers.insert("x-real-ip", "192.0.2.1, 192.0.2.2".parse().unwrap());
    assert_eq!(
        client_address("127.0.0.1".parse().unwrap(), &headers),
        "127.0.0.1".parse::<IpAddr>().unwrap()
    );
    headers.insert("x-real-ip", "192.0.2.1".parse().unwrap());
    headers.append("x-real-ip", "192.0.2.2".parse().unwrap());
    assert_eq!(
        client_address("127.0.0.1".parse().unwrap(), &headers),
        "127.0.0.1".parse::<IpAddr>().unwrap()
    );
}

#[tokio::test]
async fn http_rate_limit_has_retry_after() {
    let temp = Temp::new();
    let state = app(&temp).await;
    for _ in 0..5 {
        assert_eq!(
            router(state.clone())
                .oneshot(request(Body::from(parts(vec![meta_part()]))))
                .await
                .unwrap()
                .status(),
            StatusCode::ACCEPTED
        );
    }
    let response = router(state).oneshot(request(Body::empty())).await.unwrap();
    assert_eq!(response.status(), StatusCode::TOO_MANY_REQUESTS);
    assert!(
        response.headers()["retry-after"]
            .to_str()
            .unwrap()
            .parse::<u64>()
            .unwrap()
            >= 599
    );
}

#[tokio::test]
async fn queue_saturation_returns_503_and_permits_release() {
    let temp = Temp::new();
    let state = app(&temp).await;
    let mut active = Vec::new();
    for _ in 0..4 {
        active.push(
            state
                .gate
                .start(state.gate.reserve().unwrap(), Duration::from_secs(1))
                .await
                .unwrap(),
        );
    }
    let queued = (0..32)
        .map(|_| state.gate.reserve().unwrap())
        .collect::<Vec<_>>();
    let response = router(state.clone())
        .oneshot(request(Body::empty()))
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::SERVICE_UNAVAILABLE);
    assert!(state.gate.reserve().is_err());
    drop(queued);
    drop(active);
    assert!(
        state
            .gate
            .start(state.gate.reserve().unwrap(), Duration::from_secs(1))
            .await
            .is_ok()
    );
    let gate = Gate::new(1, 1);
    let active = gate
        .start(gate.reserve().unwrap(), Duration::from_secs(1))
        .await
        .unwrap();
    assert!(
        gate.start(gate.reserve().unwrap(), Duration::from_millis(5))
            .await
            .is_err()
    );
    assert!(gate.reserve().is_ok());
    drop(active);
}

const SYNTHETIC_DEBUG_ID: &str = "00112233445566778899AABBCCDDEEFF1";

fn synthetic(module_name: &str) -> Vec<u8> {
    use minidump::{Minidump, MinidumpModuleList, Module as _};
    use minidump_synth::{
        DumpString, Exception, Memory, Module, SynthMinidump, SystemInfo, Thread, x86_context,
    };
    use test_assembler::{Endian, Section};
    let e = Endian::Little;
    let name = DumpString::new(module_name, e);
    // RSDS with a nonzero GUID, age 1, and debug filename. rust-minidump
    // rejects a nil GUID even with a nonzero age. GUID fields are little-endian;
    // SYNTHETIC_DEBUG_ID is their Breakpad representation with the age appended.
    let cv = Section::with_endian(e)
        .append_bytes(b"RSDS")
        .D32(0x00112233)
        .D16(0x4455)
        .D16(0x6677)
        .append_bytes(&[0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff])
        .D32(1)
        .append_bytes(module_name.as_bytes())
        .D8(0);
    let module = Module::new(e, 0x400000, 0x10000, &name, 0, 0, None).cv_record(&cv);
    let context = x86_context(e, 0x401234, 0x800000);
    let stack = Memory::with_section(Section::with_endian(e).append_repeated(0, 128), 0x800000);
    let thread = Thread::new(e, 7, &stack, &context);
    let mut exception = Exception::new(e);
    exception.thread_id = 7;
    exception.exception_record.exception_code = 0xc0000005;
    exception.exception_record.exception_address = 0x401234;
    exception.exception_record.number_parameters = 2;
    exception.exception_record.exception_information[0] = 0;
    exception.exception_record.exception_information[1] = 0xdeadbeef;
    // Let the thread context drive the stack: an absent exception context is
    // supported by rust-minidump and avoids manual RVA fixups in the fixture.
    exception.thread_context = (0, 0);
    let bytes = SynthMinidump::with_endian(e)
        .add(name)
        .add(cv)
        .add(context)
        .add_module(module)
        .add_thread(thread)
        .add_memory(stack)
        .add_system_info(SystemInfo::new(e).set_platform_id(2))
        .add_exception(exception)
        .finish()
        .unwrap();
    // Validate the shared fixture for every caller before exercising the walk.
    let dump = Minidump::read(bytes.as_slice()).unwrap();
    let modules = dump.get_stream::<MinidumpModuleList>().unwrap();
    let module = modules.module_at_address(0x401234).unwrap();
    assert_eq!(
        module.debug_identifier().unwrap().breakpad().to_string(),
        SYNTHETIC_DEBUG_ID
    );
    let lookup = breakpad_symbols::breakpad_sym_lookup(module).unwrap();
    assert_eq!(
        lookup.cache_rel,
        format!("{module_name}/{SYNTHETIC_DEBUG_ID}/{module_name}.sym")
    );
    bytes
}

#[tokio::test]
async fn synthetic_crash_symbolicated_and_module_offset_events() {
    let temp = Temp::new();
    let symbols = temp.0.join("symbols");
    std::fs::create_dir(&symbols).unwrap();
    let no = worker()
        .walk(
            synthetic("SSFIV.exe"),
            symbols.clone(),
            Duration::from_secs(2),
        )
        .await;
    assert_eq!(
        no.status,
        format!("partial: symbols missing for SSFIV.exe/{SYNTHETIC_DEBUG_ID}/SSFIV.exe.sym")
    );
    assert_eq!(no.crash_frames[0].module, "SSFIV.exe");
    assert_eq!(no.crash_frames[0].offset, 0x1234);
    assert!(no.crash_frames[0].function.is_none());
    let id = "a".repeat(32);
    let event: Value = serde_json::from_slice(&event::build(&id, &report(), &no, None)).unwrap();
    assert_eq!(
        event["fingerprint"],
        json!(["{{ default }}", "SSFIV.exe+0x1234"])
    );
    assert_eq!(
        event["exception"]["values"][0]["type"],
        "EXCEPTION_ACCESS_VIOLATION_READ"
    );
    assert_eq!(
        event["exception"]["values"][0]["stacktrace"]["frames"][0]["instruction_addr"],
        "0x401234"
    );
    assert_eq!(
        event["exception"]["values"][0]["stacktrace"]["frames"][0]["in_app"],
        false
    );
    assert!(
        event["exception"]["values"][0]["value"]
            .as_str()
            .unwrap()
            .contains("0xdeadbeef")
    );
    let debug_id = SYNTHETIC_DEBUG_ID;
    let directory = symbols.join("Launcher.exe").join(debug_id);
    std::fs::create_dir_all(&directory).unwrap();
    std::fs::write(directory.join("Launcher.exe.sym"), format!("MODULE windows x86 {debug_id} Launcher.exe\nFILE 0 launcher.cpp\nFUNC 1200 100 0 report_crash\n1200 100 42 0\n")).unwrap();
    let yes = worker()
        .walk(synthetic("Launcher.exe"), symbols, Duration::from_secs(2))
        .await;
    assert_eq!(yes.status, "ok");
    assert_eq!(
        yes.crash_frames[0].function.as_deref(),
        Some("report_crash")
    );
    let event: Value = serde_json::from_slice(&event::build(
        &id,
        &report(),
        &yes,
        Some("/state/dumps/a.dmp"),
    ))
    .unwrap();
    assert!(event.get("fingerprint").is_none());
    let f = &event["exception"]["values"][0]["stacktrace"]["frames"][0];
    assert_eq!(f["function"], "report_crash");
    assert_eq!(f["filename"], "launcher.cpp");
    assert_eq!(f["lineno"], 42);
    assert_eq!(f["package"], "Launcher.exe");
    assert_eq!(f["in_app"], true);
    assert_eq!(event["extra"]["minidump_path"], "/state/dumps/a.dmp");
    assert_eq!(event["platform"], "native");
    assert_eq!(event["level"], "error");
    assert_eq!(event["release"], "0.8.0");
    assert_eq!(event["environment"], "beta");
    assert_eq!(event["tags"]["build_id"], "a".repeat(12));
}

#[cfg(target_os = "linux")]
#[tokio::test]
async fn unreadable_symbols_report_partial_and_log_only_store_relative_name() {
    use std::{
        io::Write,
        os::unix::{fs::PermissionsExt, process::CommandExt},
        process::{Command, Stdio},
    };
    let temp = Temp::new();
    // A root-run test drops only the worker's credentials, so chmod really
    // causes EACCES rather than being bypassed by root's DAC privileges.
    std::fs::set_permissions(&temp.0, std::fs::Permissions::from_mode(0o755)).unwrap();
    let relative = format!("Launcher.exe/{SYNTHETIC_DEBUG_ID}/Launcher.exe.sym");
    let path = temp.0.join(&relative);
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    for directory in [
        path.parent().unwrap(),
        path.parent().unwrap().parent().unwrap(),
    ] {
        std::fs::set_permissions(directory, std::fs::Permissions::from_mode(0o755)).unwrap();
    }
    std::fs::write(
        &path,
        format!(
            "MODULE windows x86 {SYNTHETIC_DEBUG_ID} Launcher.exe\nFUNC 1200 100 0 report_crash\n"
        ),
    )
    .unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o000)).unwrap();
    let root = unsafe { libc::geteuid() } == 0;
    let executable = if root {
        // A root-owned checkout may be under /root, which nobody cannot
        // traverse. Put only the worker binary in the accessible fixture.
        let executable = temp.0.join("symbolicate-worker");
        std::fs::copy(env!("CARGO_BIN_EXE_ember-reports"), &executable).unwrap();
        std::fs::set_permissions(&executable, std::fs::Permissions::from_mode(0o755)).unwrap();
        executable
    } else {
        PathBuf::from(env!("CARGO_BIN_EXE_ember-reports"))
    };
    let mut command = Command::new(executable);
    command
        .arg("symbolicate")
        .arg(&temp.0)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    if root {
        command.uid(65534).gid(65534);
    }
    let mut child = command.spawn().unwrap();
    child
        .stdin
        .take()
        .unwrap()
        .write_all(&synthetic("Launcher.exe"))
        .unwrap();
    let output = child.wait_with_output().unwrap();
    assert!(output.status.success());
    let walk: Walk = serde_json::from_slice(&output.stdout).unwrap();
    assert_eq!(
        walk.status,
        format!("partial: symbols unreadable for {relative}")
    );
    assert_eq!(walk.crash_frames[0].module, "Launcher.exe");
    assert_eq!(walk.crash_frames[0].offset, 0x1234);
    assert!(walk.crash_frames[0].function.is_none());
    assert_eq!(
        String::from_utf8(output.stderr).unwrap(),
        format!("ember-reports: symbols unreadable: {relative}\n")
    );
    let event: Value =
        serde_json::from_slice(&event::build(&"a".repeat(32), &report(), &walk, None)).unwrap();
    assert_eq!(event["extra"]["symbolication"], walk.status);
    assert!(!walk.status.contains(temp.0.to_str().unwrap()));
}

#[tokio::test]
async fn failed_walk_preserves_raw_facts_and_worker_slot() {
    let temp = Temp::new();
    let mut bytes = synthetic("SSFIV.exe");
    // Remove only the SystemInfo directory entry by changing its stream type.
    let count = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
    let directory = u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
    for i in 0..count {
        let p = directory + i * 12;
        if u32::from_le_bytes(bytes[p..p + 4].try_into().unwrap()) == 7 {
            bytes[p..p + 4].copy_from_slice(&0xffff_u32.to_le_bytes());
        }
    }
    let gate = Gate::new(1, 0);
    let ticket = gate
        .start(gate.reserve().unwrap(), Duration::from_secs(1))
        .await
        .unwrap();
    let walk = worker()
        .walk(bytes, temp.0.clone(), Duration::from_secs(1))
        .await;
    assert_eq!(walk.status, "walk_failed");
    assert_eq!(walk.code, Some(0xc0000005));
    assert_eq!(walk.address, Some(0xdeadbeef));
    assert_eq!(walk.crash_frames[0].offset, 0x1234);
    assert!(gate.reserve().is_err());
    drop(ticket);
    assert!(gate.reserve().is_ok());
}

#[tokio::test]
async fn input_budget_preserves_exception_without_walking_excess_threads() {
    let temp = Temp::new();
    let mut bytes = synthetic("SSFIV.exe");
    let count = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
    let directory = u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
    for i in 0..count {
        let p = directory + i * 12;
        if u32::from_le_bytes(bytes[p..p + 4].try_into().unwrap()) == 3 {
            let offset = u32::from_le_bytes(bytes[p + 8..p + 12].try_into().unwrap()) as usize;
            bytes[offset..offset + 4].copy_from_slice(&129_u32.to_le_bytes());
        }
    }
    let walk = worker()
        .walk(bytes, temp.0.clone(), Duration::from_secs(1))
        .await;
    assert_eq!(walk.status, "input_budget");
    assert_eq!(walk.code, Some(0xc0000005));
    assert_eq!(walk.address, Some(0xdeadbeef));
}

#[test]
fn event_order_thread_truncation_problem_and_size_cap() {
    let mut report = report();
    let make_frame = |i| Frame {
        instruction: i,
        module: "Sidecar.dll".into(),
        offset: i,
        function: Some(format!("frame_{i}")),
        filename: Some("source.cpp".into()),
        line: Some(10),
    };
    let walk = Walk {
        crash_frames: (0..80).map(make_frame).collect(),
        threads: vec![Thread {
            id: 2,
            frames: (100..180).map(make_frame).collect(),
        }],
        status: "ok".into(),
        ..Walk::default()
    };
    for name in intake::LOG_NAMES {
        report
            .logs
            .insert(name.into(), "\0\u{1}\"\\界".repeat(20000));
    }
    let bytes = event::build(&"b".repeat(32), &report, &walk, None);
    assert!(bytes.len() < EVENT_BYTES);
    let value: Value = serde_json::from_slice(&bytes).unwrap();
    let frames = value["exception"]["values"][0]["stacktrace"]["frames"]
        .as_array()
        .unwrap();
    assert_eq!(frames.len(), 64);
    assert_eq!(frames[0]["function"], "frame_63");
    assert_eq!(frames[63]["function"], "frame_0");
    assert_eq!(
        value["threads"]["values"][0]["stacktrace"]["frames"]
            .as_array()
            .unwrap()
            .len(),
        64
    );
    for name in intake::LOG_NAMES {
        let tail = value["extra"]["logs"][name].as_str().unwrap();
        assert!(report.logs[name].ends_with(tail));
        assert!(tail.len() < report.logs[name].len());
    }
    report.meta.kind = Kind::Problem;
    let value: Value = serde_json::from_slice(&event::build(
        &"b".repeat(32),
        &report,
        &Walk::default(),
        None,
    ))
    .unwrap();
    assert_eq!(value["message"], report.meta.comment.unwrap());
    assert_eq!(value["level"], "warning");
    assert!(value.get("exception").is_none());
}

#[tokio::test]
async fn outbox_retries_envelope_then_removes_only_delivered_event() {
    #[derive(Clone)]
    struct Fake {
        attempts: Arc<AtomicUsize>,
        seen: Arc<tokio::sync::Mutex<Vec<Vec<u8>>>>,
    }
    async fn receive(
        State(fake): State<Fake>,
        headers: axum::http::HeaderMap,
        body: axum::body::Bytes,
    ) -> StatusCode {
        assert_eq!(headers["host"], "bugs.embernetplay.link");
        assert!(
            headers["x-sentry-auth"]
                .to_str()
                .unwrap()
                .contains("sentry_key=publictestkey")
        );
        assert_eq!(headers["content-type"], "application/x-sentry-envelope");
        fake.seen.lock().await.push(body.to_vec());
        if fake.attempts.fetch_add(1, Ordering::SeqCst) < 3 {
            StatusCode::SERVICE_UNAVAILABLE
        } else {
            StatusCode::OK
        }
    }
    let fake = Fake {
        attempts: Arc::new(AtomicUsize::new(0)),
        seen: Arc::new(tokio::sync::Mutex::new(Vec::new())),
    };
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(
        axum::serve(
            listener,
            Router::new()
                .route("/api/1/envelope/", post(receive))
                .with_state(fake.clone()),
        )
        .into_future(),
    );
    let temp = Temp::new();
    let store = Store::new(temp.0.clone(), Limits::default()).await.unwrap();
    let config = Config {
        state_dir: temp.0.clone(),
        bugsink_base: format!("http://{address}"),
        ..Config::default()
    };
    let sender = Bugsink::new(&config, "publictestkey").unwrap();
    let id = "c".repeat(32);
    let event = event::build(&id, &report(), &Walk::default(), None);
    store.enqueue(&id, &event).await.unwrap();
    assert_eq!(
        sender.deliver(&store, &id).await,
        Delivery::BackendUnavailable
    );
    assert!(store.event(&id).await.unwrap().is_some());
    assert_eq!(fake.attempts.load(Ordering::SeqCst), 3);
    sender.replay(&store).await;
    assert!(store.event(&id).await.unwrap().is_none());
    let seen = fake.seen.lock().await;
    let body = seen.last().unwrap();
    let first = body.iter().position(|b| *b == b'\n').unwrap();
    let second = first + 1 + body[first + 1..].iter().position(|b| *b == b'\n').unwrap();
    assert_eq!(
        serde_json::from_slice::<Value>(&body[..first]).unwrap()["event_id"],
        id
    );
    assert_eq!(
        serde_json::from_slice::<Value>(&body[first + 1..second]).unwrap()["length"],
        event.len()
    );
    assert_eq!(&body[second + 1..body.len() - 1], event.as_slice());
    server.abort();
}

#[tokio::test]
async fn bounded_outbox_and_dump_retention_oldest_first() {
    let temp = Temp::new();
    let limits = Limits {
        outbox_count: 1,
        dump_bytes: 10,
        dump_count: 3,
        ..Limits::default()
    };
    let store = Store::new(temp.0.clone(), limits).await.unwrap();
    let a = "a".repeat(32);
    let b = "b".repeat(32);
    let c = "c".repeat(32);
    store
        .reserve(&a, 2)
        .await
        .unwrap()
        .commit(b"{}", Some(b"MDMP01"))
        .await
        .unwrap();
    assert!(store.enqueue(&b, b"{}").await.is_err());
    let first = store.dump_path(&a);
    let file = std::fs::File::options().write(true).open(&first).unwrap();
    file.set_modified(SystemTime::now() - Duration::from_secs(60))
        .unwrap();
    drop(file);
    store.delivered(&a).await.unwrap();
    store
        .reserve(&b, 2)
        .await
        .unwrap()
        .commit(b"{}", Some(b"MDMP02"))
        .await
        .unwrap();
    let second = store.dump_path(&b);
    assert!(!first.exists());
    assert!(second.exists());
    std::fs::File::options()
        .write(true)
        .open(&second)
        .unwrap()
        .set_modified(SystemTime::now() - Duration::from_secs(31 * 86400))
        .unwrap();
    std::fs::write(temp.0.join("dumps/operator-notes.txt"), "keep").unwrap();
    store.prune_dumps(SystemTime::now()).await.unwrap();
    assert!(!second.exists());
    assert!(temp.0.join("dumps/operator-notes.txt").exists());
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(
            std::fs::metadata(temp.0.join("outbox").join(format!("{b}.json")))
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o600
        );
    }
    store.delivered(&b).await.unwrap();
    store
        .reserve(&c, 2)
        .await
        .unwrap()
        .commit(b"{}", Some(b"MDMP03"))
        .await
        .unwrap();
    assert!(store.dump_path(&c).exists());
}

#[test]
fn config_rejects_public_bind_and_remote_or_credentialled_bugsink() {
    let defaults = || Config {
        state_dir: std::env::temp_dir().join("ember-reports"),
        ..Config::default()
    };
    let mut config = defaults();
    assert!(config.validate().is_ok());
    for address in ["0.0.0.0:47850", "[::1]:47850", "127.0.0.2:47850"] {
        config.bind = address.parse().unwrap();
        assert!(config.validate().is_err());
    }
    config = defaults();
    for url in [
        "https://127.0.0.1:47860",
        "http://example.com",
        "http://key@127.0.0.1:47860",
        "http://127.0.0.1:47860/other",
    ] {
        config.bugsink_base = url.into();
        assert!(config.validate().is_err());
    }
    config = defaults();
    config.limits.workers = 5;
    assert!(config.validate().is_err());
    config = defaults();
    config.limits.queue = 33;
    assert!(config.validate().is_err());
}

#[test]
fn config_rejects_a_bind_port_nginx_does_not_proxy_to() {
    let mut config = Config {
        state_dir: std::env::temp_dir().join("ember-reports"),
        ..Config::default()
    };
    for port in [0u16, 1, 47851, 47860] {
        config.bind = format!("127.0.0.1:{port}").parse().unwrap();
        assert!(config.validate().is_err(), "{port}");
    }
    config.bind = format!("127.0.0.1:{}", ember_reports::config::BIND_PORT)
        .parse()
        .unwrap();
    assert!(config.validate().is_ok());
}

fn deploy_file(path: &str) -> String {
    std::fs::read_to_string(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join(path)).unwrap()
}

#[test]
fn nginx_proxies_to_the_validated_port_with_headers_on_every_response() {
    let locations = deploy_file("deploy/nginx/ember-reports-locations.conf");
    let upstream = format!("proxy_pass http://127.0.0.1:{};", ember_reports::config::BIND_PORT);
    assert!(locations.contains(&upstream), "proxy_pass must match BIND_PORT");
    assert!(deploy_file("deploy/config.example.json")
        .contains(&format!("127.0.0.1:{}", ember_reports::config::BIND_PORT)));
    // Each location that sets its own add_header (the proxy and the 429
    // named location) must include the common headers, or nginx drops the
    // server's.
    let blocks: Vec<&str> = locations.split("\nlocation ").skip(1).collect();
    assert_eq!(blocks.len(), 2);
    for block in blocks {
        assert!(block.contains("add_header"));
        assert!(
            block.contains("include snippets/ember-reports-headers.conf;"),
            "{block}"
        );
    }
    let ours = deploy_file("deploy/nginx/ember-reports-headers.conf");
    let shared = deploy_file("../ember-short/deploy/nginx/ember-short-headers.conf");
    let directives = |text: &str| -> Vec<String> {
        text.lines()
            .filter(|line| line.starts_with("add_header"))
            .map(str::to_owned)
            .collect()
    };
    assert_eq!(directives(&ours).len(), 3);
    assert_eq!(directives(&ours), directives(&shared));
    let setup = deploy_file("deploy/setup.sh");
    assert!(setup.contains("nginx/ember-reports-headers.conf"));
    assert!(setup.contains(r#""$LOCATIONS" "$HEADERS""#));
}
