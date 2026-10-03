//! `!room` over Twitch's EventSub WebSocket, against a local WebSocket server,
//! a mock Helix and a mock bridge.
mod support;

use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
    time::Duration,
};

use ember_notifier::{Config, Twitch};
use ember_protocol::{EmberId, rooms::RoomCreator};
use futures_util::{SinkExt, StreamExt};
use serde_json::{Value, json};
use support::{Mock, PLAYER, ROOM_ID, base_config, bot, bridge, notifier, refusal, room};
use tokio::net::{TcpListener, TcpStream};
use tokio_websockets::{Message, ServerBuilder, WebSocketStream};

type Ws = WebSocketStream<TcpStream>;

fn frame(kind: &str, id: &str, payload: Value) -> Message {
    Message::text(
        json!({
            "metadata": {
                "message_id": id, "message_type": kind,
                "message_timestamp": "2026-10-03T00:00:00.000000000Z",
                "subscription_type": "channel.chat.message", "subscription_version": "1",
            },
            "payload": payload,
        })
        .to_string(),
    )
}

fn welcome(id: &str, session: &str, keepalive: u64) -> Message {
    frame(
        "session_welcome",
        id,
        json!({ "session": { "id": session, "status": "connected", "keepalive_timeout_seconds": keepalive } }),
    )
}

fn chat(id: &str, text: &str, badges: &[&str]) -> Message {
    frame(
        "notification",
        id,
        json!({
            "subscription": { "type": "channel.chat.message" },
            "event": {
                "broadcaster_user_id": "1001",
                "chatter_user_id": "3003",
                "message_id": format!("chat-{id}"),
                "message": { "text": text, "fragments": [] },
                "badges": badges.iter().map(|set| json!({ "set_id": set, "id": "1", "info": "" })).collect::<Vec<_>>(),
            },
        }),
    )
}

/// A WebSocket server that hands each accepted connection, with its index, to
/// `handler`.
async fn ws_server<F, Fut>(handler: F) -> String
where
    F: Fn(usize, Ws, String) -> Fut + Send + Sync + 'static,
    Fut: Future<Output = ()> + Send + 'static,
{
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let url = format!("ws://{}", listener.local_addr().unwrap());
    let handler = Arc::new(handler);
    let base = url.clone();
    tokio::spawn(async move {
        for index in 0..usize::MAX {
            let (stream, _) = listener.accept().await.unwrap();
            let (handler, base) = (handler.clone(), base.clone());
            tokio::spawn(async move {
                if let Ok((_, ws)) = ServerBuilder::new().accept(stream).await {
                    handler(index, ws, base).await;
                }
            });
        }
    });
    url
}

/// Helix: accepts subscriptions and chat messages.
async fn helix() -> Mock {
    Mock::start(|request| match request.path.as_str() {
        "/helix/eventsub/subscriptions" => (202, "{}".into()),
        "/helix/chat/messages" => (
            200,
            r#"{"data":[{"message_id":"m","is_sent":true,"drop_reason":null}]}"#.into(),
        ),
        _ => (404, "{}".into()),
    })
    .await
}

fn config(helix: &Mock, bridge: &Mock, eventsub_url: &str) -> Config {
    let mut config = base_config("twitch-room");
    config.bot = Some(bot(&bridge.origin));
    config.twitch = Some(Twitch {
        api_base: helix.origin.clone(),
        client_id: "test-client".into(),
        token: "test-token".into(),
        broadcaster_id: "1001".into(),
        sender_id: "2002".into(),
        room_creator: Some(RoomCreator {
            participant_id: "par_1".into(),
            ember_id: EmberId::parse(PLAYER).unwrap(),
        }),
        eventsub_url: eventsub_url.into(),
    });
    config
}

async fn until(mut done: impl FnMut() -> bool) {
    for _ in 0..400 {
        if done() {
            return;
        }
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    panic!("timed out");
}

fn chats(helix: &Mock) -> Vec<Value> {
    helix
        .requests()
        .into_iter()
        .filter(|r| r.path == "/helix/chat/messages")
        .map(|r| r.body)
        .collect()
}

fn subscriptions(helix: &Mock) -> Vec<Value> {
    helix
        .requests()
        .into_iter()
        .filter(|r| r.path == "/helix/eventsub/subscriptions")
        .map(|r| r.body)
        .collect()
}

fn link() -> String {
    format!(
        "https://embernetplay.link/r#{}/{ROOM_ID}",
        support::BRIDGE_ID
    )
}

#[tokio::test]
async fn a_moderator_opens_a_room_and_the_session_survives_a_reconnect_and_a_drop() {
    // The first request opens the room; the creator has it afterwards.
    let creates = Arc::new(AtomicUsize::new(0));
    let counter = creates.clone();
    let bridge = bridge(
        move || {
            if counter.fetch_add(1, Ordering::SeqCst) == 0 {
                (201, room("waiting", 0).to_string())
            } else {
                (
                    409,
                    refusal(
                        "stale_revision",
                        json!({ "reason": "room_limit", "room_id": ROOM_ID }),
                    ),
                )
            }
        },
        || (200, room("open", 2).to_string()),
    )
    .await;
    let helix = helix().await;
    let old_closed = Arc::new(Mutex::new(false));
    let closed = old_closed.clone();
    let url = ws_server(move |index, mut ws, base| {
        let closed = closed.clone();
        async move {
            match index {
                0 => {
                    ws.send(welcome("w1", "sess1", 10)).await.unwrap();
                    // A viewer's request, a moderator's, a repeat of it and the
                    // broadcaster's.
                    ws.send(chat("c0", "!room", &["subscriber"])).await.unwrap();
                    ws.send(chat("c1", "!room", &["moderator"])).await.unwrap();
                    ws.send(chat("c1", "!room", &["moderator"])).await.unwrap();
                    ws.send(chat("c2", "!room", &["broadcaster"])).await.unwrap();
                    ws.send(frame("session_keepalive", "k1", json!({}))).await.unwrap();
                    tokio::time::sleep(Duration::from_millis(600)).await;
                    ws.send(frame(
                        "session_reconnect",
                        "r1",
                        json!({ "session": { "id": "sess1", "reconnect_url": format!("{base}/?reconnect=1") } }),
                    ))
                    .await
                    .unwrap();
                    // The client drops this connection once the new one has
                    // welcomed it.
                    while ws.next().await.is_some_and(|message| message.is_ok()) {}
                    *closed.lock().unwrap() = true;
                }
                1 => {
                    // The same session carries over: no new subscription.
                    ws.send(welcome("w2", "sess1", 10)).await.unwrap();
                    ws.send(chat("c3", "!room", &["moderator"])).await.unwrap();
                    tokio::time::sleep(Duration::from_millis(600)).await;
                    // A drop with no close frame.
                    drop(ws);
                }
                _ => {
                    ws.send(welcome("w3", "sess3", 10)).await.unwrap();
                    while ws.next().await.is_some() {}
                }
            }
        }
    })
    .await;
    let (_, tasks) = notifier(config(&helix, &bridge, &url))
        .start(TcpListener::bind("127.0.0.1:0").await.unwrap())
        .unwrap();

    // The room, its link again for the broadcaster, and again after the
    // reconnect; the viewer's request and the repeat were ignored.
    until(|| chats(&helix).len() == 3).await;
    let posted = chats(&helix);
    for text in posted.iter().map(|body| body["message"].as_str().unwrap()) {
        assert!(text.contains(&link()), "{text}");
    }
    assert_eq!(posted[0]["broadcaster_id"], "1001");
    assert_eq!(posted[0]["sender_id"], "2002");
    assert!(posted[1]["message"].as_str().unwrap().contains("(2/8)"));
    assert_eq!(creates.load(Ordering::SeqCst), 3);
    let create = bridge
        .requests()
        .into_iter()
        .find(|r| r.path == "/v1/rooms")
        .unwrap();
    assert_eq!(
        create.body["creator"],
        json!({ "participant_id": "par_1", "ember_id": PLAYER })
    );

    // After the drop the next session subscribes again; the reconnect did not.
    until(|| subscriptions(&helix).len() == 2).await;
    let subscribed = subscriptions(&helix);
    assert_eq!(
        subscribed[0],
        json!({
            "type": "channel.chat.message", "version": "1",
            "condition": { "broadcaster_user_id": "1001", "user_id": "2002" },
            "transport": { "method": "websocket", "session_id": "sess1" },
        })
    );
    assert_eq!(subscribed[1]["transport"]["session_id"], "sess3");
    let first = helix
        .requests()
        .into_iter()
        .find(|r| r.path == "/helix/eventsub/subscriptions")
        .unwrap();
    assert_eq!(first.headers["authorization"], "Bearer test-token");
    assert_eq!(first.headers["client-id"], "test-client");
    assert!(*old_closed.lock().unwrap());
    for task in tasks {
        task.abort();
    }
}

#[tokio::test]
async fn keepalives_hold_a_session_and_silence_ends_it() {
    let bridge = bridge(support::created, support::no_room).await;
    let helix = helix().await;
    let connections = Arc::new(AtomicUsize::new(0));
    let count = connections.clone();
    let url = ws_server(move |index, mut ws, _| {
        count.fetch_add(1, Ordering::SeqCst);
        async move {
            // A one second timeout, and keepalives for longer than that and
            // its grace: the session must stay.
            ws.send(welcome(&format!("w{index}"), &format!("sess{index}"), 1))
                .await
                .unwrap();
            if index == 0 {
                for n in 0..8 {
                    tokio::time::sleep(Duration::from_millis(500)).await;
                    ws.send(frame("session_keepalive", &format!("k{n}"), json!({})))
                        .await
                        .unwrap();
                }
            }
            // Then silence, with the connection still open.
            tokio::time::sleep(Duration::from_secs(30)).await;
            drop(ws);
        }
    })
    .await;
    let (_, tasks) = notifier(config(&helix, &bridge, &url))
        .start(TcpListener::bind("127.0.0.1:0").await.unwrap())
        .unwrap();
    until(|| subscriptions(&helix).len() == 1).await;
    tokio::time::sleep(Duration::from_millis(3500)).await;
    assert_eq!(connections.load(Ordering::SeqCst), 1);
    // Silence: the timeout and grace pass, then a reconnect and a new
    // subscription.
    until(|| subscriptions(&helix).len() == 2).await;
    assert_eq!(connections.load(Ordering::SeqCst), 2);
    for task in tasks {
        task.abort();
    }
}

#[tokio::test]
async fn a_refused_subscription_is_retried_and_other_chat_is_ignored() {
    let bridge = bridge(support::created, support::no_room).await;
    let refusals = Arc::new(AtomicUsize::new(0));
    let counter = refusals.clone();
    let helix = Mock::start(move |request| match request.path.as_str() {
        "/helix/eventsub/subscriptions" if counter.fetch_add(1, Ordering::SeqCst) == 0 => {
            (401, "{}".into())
        }
        "/helix/eventsub/subscriptions" => (202, "{}".into()),
        _ => (
            200,
            r#"{"data":[{"message_id":"m","is_sent":true,"drop_reason":null}]}"#.into(),
        ),
    })
    .await;
    let url = ws_server(|index, mut ws, _| async move {
        ws.send(welcome(&format!("w{index}"), &format!("sess{index}"), 10))
            .await
            .unwrap();
        if index == 1 {
            // Another channel's chat, and a message that is not the command.
            let mut elsewhere = chat("e1", "!room", &["moderator"]);
            if let Some(text) = elsewhere.as_text() {
                elsewhere = Message::text(text.replace("\"1001\"", "\"9999\""));
            }
            ws.send(elsewhere).await.unwrap();
            ws.send(chat("e2", "hello", &["moderator"])).await.unwrap();
        }
        while ws.next().await.is_some() {}
    })
    .await;
    let (_, tasks) = notifier(config(&helix, &bridge, &url))
        .start(TcpListener::bind("127.0.0.1:0").await.unwrap())
        .unwrap();
    // The first subscription is refused (401); the second session's is not.
    until(|| refusals.load(Ordering::SeqCst) == 2).await;
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert!(chats(&helix).is_empty());
    assert!(bridge.requests().iter().all(|r| r.path != "/v1/rooms"));
    for task in tasks {
        task.abort();
    }
}
