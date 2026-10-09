mod support;
use axum::{
    body::{Body, to_bytes},
    extract::ConnectInfo,
    http::{Request, StatusCode},
};
use ember_reports::{
    App,
    config::{Config, Limits},
    intake,
    limits::{Gate, RateLimiter, client_address},
    router,
};
use serde_json::{Value, json};
use std::{
    net::{IpAddr, SocketAddr},
    sync::Arc,
    time::{Duration, Instant},
};
use support::{Temp, id, meta, worker};
use tower::ServiceExt;

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
    let stored = state.store.event(&support::id(id)).await.unwrap().unwrap();
    let event: Value = serde_json::from_slice(&stored).unwrap();
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
    let accepted = id(&"a".repeat(32));
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

// The reason strings are the launcher's contract, not only the status codes.
#[tokio::test]
async fn refusals_send_their_wire_reason() {
    async fn reason(state: &Arc<App>, request: Request<Body>) -> (StatusCode, String) {
        let response = router(state.clone()).oneshot(request).await.unwrap();
        let status = response.status();
        let body: Value =
            serde_json::from_slice(&to_bytes(response.into_body(), 1024).await.unwrap()).unwrap();
        (status, body["reason"].as_str().unwrap().to_owned())
    }
    let temp = Temp::new();
    let state = App::with_worker(
        Config {
            state_dir: temp.0.clone(),
            limits: Limits {
                outbox_count: 1,
                ..Limits::default()
            },
            ..Config::default()
        },
        "publictestkey",
        worker(),
    )
    .await
    .unwrap();
    let cases = [
        (
            vec![("meta", None, b"{".to_vec())],
            StatusCode::BAD_REQUEST,
            "invalid_meta_json",
        ),
        (
            vec![meta_part(), ("other", None, vec![])],
            StatusCode::BAD_REQUEST,
            "unknown_part",
        ),
        (
            vec![
                meta_part(),
                ("log", Some("sf4e.log"), vec![b'x'; intake::LOG_BYTES + 1]),
            ],
            StatusCode::PAYLOAD_TOO_LARGE,
            "part_too_large",
        ),
    ];
    for (input, status, expected) in cases {
        assert_eq!(
            reason(&state, request(Body::from(parts(input)))).await,
            (status, expected.to_owned())
        );
    }
    state
        .store
        .enqueue(&id(&"a".repeat(32)), b"{}")
        .await
        .unwrap();
    assert_eq!(
        reason(&state, request(Body::from(parts(vec![meta_part()])))).await,
        (StatusCode::SERVICE_UNAVAILABLE, "outbox_unavailable".into())
    );
    let mut unaddressed = request(Body::empty());
    unaddressed.extensions_mut().clear();
    assert_eq!(
        reason(&state, unaddressed).await,
        (StatusCode::INTERNAL_SERVER_ERROR, "missing_peer".into())
    );
    // Fill every worker and queue slot, then use the fifth and sixth requests
    // from this address.
    let held = (0..4 + 32)
        .map(|_| state.gate.reserve().unwrap())
        .collect::<Vec<_>>();
    assert_eq!(
        reason(&state, request(Body::empty())).await,
        (StatusCode::SERVICE_UNAVAILABLE, "queue_full".into())
    );
    assert_eq!(
        reason(&state, request(Body::empty())).await,
        (StatusCode::TOO_MANY_REQUESTS, "rate_limit".into())
    );
    drop(held);
}
