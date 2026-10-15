mod support;
use axum::{Router, body::Bytes, extract::State, http::StatusCode, routing::post};
use ember_reports::{
    bugsink::{Bugsink, Delivery},
    config::{Config, Limits},
    event::{self, EVENT_BYTES},
    store::Store,
    symbolicate::Walk,
};
use serde_json::Value;
use std::{
    future::IntoFuture,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
};
use support::{Temp, id, report};

async fn backend(
    status: StatusCode,
) -> (
    Bugsink,
    Arc<AtomicUsize>,
    tokio::task::JoinHandle<std::io::Result<()>>,
) {
    async fn receive(
        State((status, attempts)): State<(StatusCode, Arc<AtomicUsize>)>,
        body: Bytes,
    ) -> StatusCode {
        attempts.fetch_add(1, Ordering::SeqCst);
        if body.ends_with(b"{\"valid\":true}\n") {
            StatusCode::OK
        } else {
            status
        }
    }
    let attempts = Arc::new(AtomicUsize::new(0));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(
        axum::serve(
            listener,
            Router::new()
                .route("/api/1/envelope/", post(receive))
                .with_state((status, attempts.clone())),
        )
        .into_future(),
    );
    let sender = Bugsink::new(
        &Config {
            bugsink_base: format!("http://{address}"),
            ..Config::default()
        },
        "publictestkey",
    )
    .unwrap();
    (sender, attempts, server)
}

#[tokio::test]
async fn permanent_and_unreadable_old_entries_rotate_past_a_full_batch() {
    let temp = Temp::new();
    let store = Store::new(temp.0.clone(), Limits::default()).await.unwrap();
    // One unreadable event plus 16 permanent failures previously starved all
    // later reports even if replay merely changed break to continue.
    let unreadable = id(&format!("{:032x}", 0));
    store.enqueue(&unreadable, b"{}").await.unwrap();
    std::fs::write(
        temp.0.join("outbox").join(format!("{unreadable}.json")),
        vec![b'x'; EVENT_BYTES],
    )
    .unwrap();
    for i in 1..=16 {
        store
            .enqueue(&id(&format!("{i:032x}")), b"{}")
            .await
            .unwrap();
    }
    let valid = id(&"f".repeat(32));
    store.enqueue(&valid, b"{\"valid\":true}").await.unwrap();
    let (sender, attempts, server) = backend(StatusCode::BAD_REQUEST).await;
    assert_eq!(
        sender.deliver(&store, &unreadable).await,
        Delivery::ReadFailed
    );
    assert_eq!(
        sender.deliver(&store, &id(&format!("{:032x}", 1))).await,
        Delivery::Rejected(400)
    );
    assert_eq!(attempts.load(Ordering::SeqCst), 1);
    sender.replay(&store).await;
    assert!(store.event(&valid).await.unwrap().is_some());
    sender.replay(&store).await;
    assert!(store.event(&valid).await.unwrap().is_none());
    assert!(store.event(&unreadable).await.is_err());
    for i in 1..=16 {
        assert_eq!(
            store
                .event(&id(&format!("{i:032x}")))
                .await
                .unwrap()
                .unwrap(),
            b"{}"
        );
    }
    assert_eq!(
        std::fs::read_dir(temp.0.join("outbox")).unwrap().count(),
        17
    );
    assert_eq!(
        sender.deliver(&store, &valid).await,
        Delivery::AlreadyDelivered
    );
    server.abort();
}

#[tokio::test]
async fn backend_outage_pauses_batch_and_preserves_all_events() {
    for status in [
        StatusCode::SERVICE_UNAVAILABLE,
        StatusCode::UNAUTHORIZED,
        StatusCode::TOO_MANY_REQUESTS,
    ] {
        let temp = Temp::new();
        let store = Store::new(temp.0.clone(), Limits::default()).await.unwrap();
        for i in 0..18 {
            store
                .enqueue(&id(&format!("{i:032x}")), b"{}")
                .await
                .unwrap();
        }
        let (sender, attempts, server) = backend(status).await;
        sender.replay(&store).await;
        assert_eq!(attempts.load(Ordering::SeqCst), 3);
        assert_eq!(
            std::fs::read_dir(temp.0.join("outbox")).unwrap().count(),
            18
        );
        server.abort();
    }
}

// Unlike backend(), this checks the authentication and envelope headers,
// records every body, and fails the first three attempts before accepting.
#[derive(Clone)]
struct Recorder {
    attempts: Arc<AtomicUsize>,
    seen: Arc<tokio::sync::Mutex<Vec<Vec<u8>>>>,
}
async fn recording_backend() -> (
    Bugsink,
    Recorder,
    tokio::task::JoinHandle<std::io::Result<()>>,
) {
    async fn receive(
        State(fake): State<Recorder>,
        headers: axum::http::HeaderMap,
        body: Bytes,
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
    let fake = Recorder {
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
    let sender = Bugsink::new(
        &Config {
            bugsink_base: format!("http://{address}"),
            ..Config::default()
        },
        "publictestkey",
    )
    .unwrap();
    (sender, fake, server)
}

#[tokio::test]
async fn outbox_retries_envelope_then_removes_only_delivered_event() {
    let (sender, fake, server) = recording_backend().await;
    let temp = Temp::new();
    let store = Store::new(temp.0.clone(), Limits::default()).await.unwrap();
    let id = id(&"c".repeat(32));
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
        id.as_str()
    );
    assert_eq!(
        serde_json::from_slice::<Value>(&body[first + 1..second]).unwrap()["length"],
        event.len()
    );
    assert_eq!(&body[second + 1..body.len() - 1], event.as_slice());
    server.abort();
}
