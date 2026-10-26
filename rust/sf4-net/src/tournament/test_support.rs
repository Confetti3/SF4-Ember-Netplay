//! Fixtures the tournament modules' tests share: a helper run's shared state
//! and a bridge that answers its capabilities.
use std::sync::{
    Arc,
    atomic::{AtomicU8, AtomicUsize, Ordering},
};

use ember_protocol::play::{CLAIM_READY_FEATURE, WATCH_FEATURE};
use serde_json::{Value, json};

use super::{Shared, builtin::Builtin, client, play::State};

/// A bridge's `/v1/capabilities` answer, listing `features`.
pub(super) fn capabilities_json(features: &[&str]) -> Value {
    json!({
        "api_version": "v1", "event_version": "v1", "games": ["usf4"], "games_to_win": [1],
        "native_rules_profiles": [], "native_play": true, "result_sources": [],
        "features": features,
        "limits": {
            "max_body_bytes": 1, "max_proof_body_bytes": 1, "max_json_depth": 1,
            "challenge_lifetime_secs": 1, "session_lifetime_secs": 1, "link_code_lifetime_secs": 1,
        },
    })
}

/// A bridge origin on loopback whose `/v1/capabilities` answers as `mode`
/// says: 0 fails, 1 lists neither `ready` nor watching, 2 lists both. It
/// answers every path so, and counts each request.
pub(super) async fn capabilities_server(mode: Arc<AtomicU8>) -> (String, Arc<AtomicUsize>) {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    let asked = Arc::new(AtomicUsize::new(0));
    let counted = asked.clone();
    tokio::spawn(async move {
        loop {
            let Ok((mut socket, _)) = listener.accept().await else {
                return;
            };
            let mut request = Vec::new();
            let mut buffer = [0; 1024];
            while !request.windows(4).any(|end| end == b"\r\n\r\n") {
                match socket.read(&mut buffer).await {
                    Ok(0) | Err(_) => break,
                    Ok(read) => request.extend_from_slice(&buffer[..read]),
                }
            }
            counted.fetch_add(1, Ordering::SeqCst);
            let features: &[&str] = match mode.load(Ordering::SeqCst) {
                1 => &["matches.play"],
                2 => &["matches.play", CLAIM_READY_FEATURE, WATCH_FEATURE],
                _ => {
                    let _ = socket
                        .write_all(b"HTTP/1.1 503 Service Unavailable\r\ncontent-length: 0\r\nconnection: close\r\n\r\n")
                        .await;
                    continue;
                }
            };
            let body = serde_json::to_vec(&capabilities_json(features)).unwrap();
            let head = format!(
                "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n",
                body.len()
            );
            let _ = socket.write_all(head.as_bytes()).await;
            let _ = socket.write_all(&body).await;
        }
    });
    (origin, asked)
}

/// A helper run's shared state in `dir`, pinning `builtins`.
pub(super) fn shared_pinning(dir: &std::path::Path, builtins: &[Builtin]) -> Arc<Shared> {
    Arc::new(Shared {
        identity: std::sync::Mutex::new(crate::identity::Identity::open(
            dir.join("identity"),
            false,
            crate::identity::Cost::DEFAULT,
        )),
        bridges: std::sync::Mutex::new(super::bridges::Store::open(dir.join("bridges"), builtins)),
        client: client::Client::new(),
        http: Arc::new(tokio::sync::Semaphore::new(1)),
        play: std::sync::Mutex::new(State::default()),
        spool: super::spool::Spool::open(dir.join("reports")),
        endpoint_id: "a".repeat(64),
        instance_id: ember_protocol::encoding::prefixed_id("ins", [1; 16]),
        home_region: Box::new(|| None),
    })
}

/// A bridge on loopback that reports each request's first line, holds the
/// first one until it is let go, and answers every request with `reply`.
pub(super) fn hold_first_request(
    listener: tokio::net::TcpListener,
    reply: String,
) -> (
    tokio::sync::mpsc::UnboundedReceiver<String>,
    tokio::sync::oneshot::Sender<()>,
) {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    let (held, asked) = tokio::sync::mpsc::unbounded_channel();
    let (release, released) = tokio::sync::oneshot::channel::<()>();
    tokio::spawn(async move {
        let mut released = Some(released);
        while let Ok((mut socket, _)) = listener.accept().await {
            let mut request = Vec::new();
            let mut buffer = [0; 1024];
            while !request.windows(4).any(|end| end == b"\r\n\r\n") {
                match socket.read(&mut buffer).await {
                    Ok(0) | Err(_) => break,
                    Ok(read) => request.extend_from_slice(&buffer[..read]),
                }
            }
            let line = String::from_utf8_lossy(&request)
                .lines()
                .next()
                .unwrap_or_default()
                .to_owned();
            let _ = held.send(line);
            if let Some(released) = released.take() {
                let _ = released.await;
            }
            let _ = socket.write_all(reply.as_bytes()).await;
        }
    });
    (asked, release)
}

/// A JSON answer with `status`, as a bridge sends it.
pub(super) fn json_reply(status: &str, body: &str) -> String {
    format!(
        "HTTP/1.1 {status}\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}",
        body.len()
    )
}
