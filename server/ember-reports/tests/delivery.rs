mod support;
use axum::{Router, body::Bytes, extract::State, http::StatusCode, routing::post};
use ember_reports::{
    bugsink::{Bugsink, Delivery},
    config::{Config, Limits},
    event::EVENT_BYTES,
    store::Store,
};
use std::{
    future::IntoFuture,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
};
use support::Temp;

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
    let unreadable = format!("{:032x}", 0);
    store.enqueue(&unreadable, b"{}").await.unwrap();
    std::fs::write(
        temp.0.join("outbox").join(format!("{unreadable}.json")),
        vec![b'x'; EVENT_BYTES],
    )
    .unwrap();
    for i in 1..=16 {
        store.enqueue(&format!("{i:032x}"), b"{}").await.unwrap();
    }
    let valid = "f".repeat(32);
    store.enqueue(&valid, b"{\"valid\":true}").await.unwrap();
    let (sender, attempts, server) = backend(StatusCode::BAD_REQUEST).await;
    assert_eq!(
        sender.deliver(&store, &unreadable).await,
        Delivery::ReadFailed
    );
    assert_eq!(
        sender.deliver(&store, &format!("{:032x}", 1)).await,
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
            store.event(&format!("{i:032x}")).await.unwrap().unwrap(),
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
            store.enqueue(&format!("{i:032x}"), b"{}").await.unwrap();
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
