//! Shared by the room bot tests: a recording mock HTTP service, and the room
//! and player objects the bridge answers with.
#![allow(dead_code)]
use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
    time::Duration,
};

use axum::{
    Router,
    body::Bytes,
    extract::State,
    http::{HeaderMap, Method, StatusCode, Uri, header},
    response::{IntoResponse, Response},
};
use ember_notifier::{Bot, Clock, Config, Notifier};
use serde_json::{Value, json};

pub const PLAYER: &str = "emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";
pub const ROOM_ID: &str = "0123456789abcdef0123456789abcdef";
pub const BRIDGE_ID: &str = "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11";
pub const SECRET: &str = "whsec_C2FVsBQIhrscChlQIMV+b5sSYspob7oLkPBEB3QKLYQ=";

pub fn temp(name: &str) -> PathBuf {
    let mut bytes = [0u8; 8];
    getrandom::fill(&mut bytes).unwrap();
    let dir = std::env::temp_dir().join(format!(
        "ember-notifier-{name}-{}",
        ember_protocol::encoding::b64u(&bytes)
    ));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

#[derive(Clone, Debug)]
pub struct Logged {
    pub method: Method,
    pub path: String,
    pub headers: HeaderMap,
    pub body: Value,
}

type Answer = Box<dyn Fn(&Logged) -> (u16, String) + Send + Sync>;

/// An HTTP service on a loopback port that logs every request and answers
/// from a function.
pub struct Mock {
    pub origin: String,
    pub log: Arc<Mutex<Vec<Logged>>>,
}

struct MockState {
    log: Arc<Mutex<Vec<Logged>>>,
    answer: Answer,
}

impl Mock {
    pub async fn start(answer: impl Fn(&Logged) -> (u16, String) + Send + Sync + 'static) -> Self {
        let log = Arc::new(Mutex::new(Vec::new()));
        let state = Arc::new(MockState {
            log: log.clone(),
            answer: Box::new(answer),
        });
        let app = Router::new().fallback(handle).with_state(state);
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let origin = format!("http://{}", listener.local_addr().unwrap());
        tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
        Self { origin, log }
    }

    pub fn requests(&self) -> Vec<Logged> {
        self.log.lock().unwrap().clone()
    }

    /// Waits for a request whose path ends with `suffix`.
    pub async fn wait_for(&self, method: Method, suffix: &str) -> Logged {
        for _ in 0..200 {
            if let Some(found) = self
                .requests()
                .into_iter()
                .find(|request| request.method == method && request.path.ends_with(suffix))
            {
                return found;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        panic!("no {method} {suffix}: {:?}", self.requests());
    }
}

async fn handle(
    State(state): State<Arc<MockState>>,
    method: Method,
    uri: Uri,
    headers: HeaderMap,
    body: Bytes,
) -> Response {
    let logged = Logged {
        method,
        path: uri.path().to_owned(),
        headers,
        body: serde_json::from_slice(&body).unwrap_or(Value::Null),
    };
    let (status, text) = (state.answer)(&logged);
    state.log.lock().unwrap().push(logged);
    (
        StatusCode::from_u16(status).unwrap(),
        [(header::CONTENT_TYPE, "application/json")],
        text,
    )
        .into_response()
}

pub fn room(state: &str, members: u8) -> Value {
    json!({
        "room": {
            "room_id": ROOM_ID,
            "name": "Fight Night",
            "build_id": "test-build",
            "members": members,
            "capacity": 8,
            "tables_playing": 0,
            "region": "use1",
            "created_at": 1_800_000_000u64,
        },
        "state": state,
        "creator_ember_id": PLAYER,
        "join_url": format!("https://embernetplay.link/r#{BRIDGE_ID}/{ROOM_ID}"),
    })
}

pub fn found_player(discord_id: &str) -> Value {
    json!({ "discord_user_id": discord_id, "ember_id": PLAYER, "participant_id": "par_1" })
}

/// The bridge's error shape.
pub fn refusal(code: &str, details: Value) -> String {
    json!({ "error": {
        "code": code, "message": "Refused.", "retryable": false,
        "request_id": "req_1", "details": details,
    } })
    .to_string()
}

pub fn profile(origin: &str) -> String {
    json!({
        "bridge_id": BRIDGE_ID,
        "origin": origin,
        "display_name": "Test bridge",
        "api_versions": ["v1"],
        "capabilities_url": format!("{origin}/v1/capabilities"),
        "signing_keys_url": format!("{origin}/v1/signing-keys"),
    })
    .to_string()
}

pub fn bot(bridge_api: &str) -> Bot {
    Bot {
        bridge_api: bridge_api.into(),
        credential: "test-credential".into(),
        build_id: "test-build".into(),
        capacity: 8,
        room_name: Some("Fight Night".into()),
    }
}

/// A notifier config with nothing to post and no bot.
pub fn base_config(name: &str) -> Config {
    Config {
        listen: "127.0.0.1:0".into(),
        bridge_origin: "http://127.0.0.1:1".into(),
        secrets: vec![SECRET.into()],
        database: temp(name).join("inbox.sqlite3"),
        discord: None,
        twitch: None,
        bot: None,
        names: Default::default(),
    }
}

pub fn notifier(config: Config) -> Notifier {
    Notifier::new(config, Clock::default()).unwrap()
}

/// A mock bridge whose lookup finds Discord user `42` and whose room routes
/// answer from `create` and `get`.
pub async fn bridge(
    create: impl Fn() -> (u16, String) + Send + Sync + 'static,
    get: impl Fn() -> (u16, String) + Send + Sync + 'static,
) -> Mock {
    let origin = std::sync::Arc::new(std::sync::Mutex::new(String::new()));
    let seen = origin.clone();
    let mock = Mock::start(move |request: &Logged| {
        match (request.method.as_str(), request.path.as_str()) {
            ("GET", "/.well-known/ember-bridge.json") => (200, profile(&seen.lock().unwrap())),
            ("POST", "/v1/players/lookup") => {
                let asked = request.body["discord"][0].as_str().unwrap_or_default();
                let players = if asked == "42" {
                    vec![found_player("42")]
                } else {
                    vec![]
                };
                (200, json!({ "players": players }).to_string())
            }
            ("POST", "/v1/rooms") => create(),
            ("GET", path) if path.starts_with("/v1/rooms/") => get(),
            _ => (404, "{}".into()),
        }
    })
    .await;
    *origin.lock().unwrap() = mock.origin.clone();
    mock
}

pub fn created() -> (u16, String) {
    (201, room("waiting", 0).to_string())
}

pub fn no_room() -> (u16, String) {
    (404, "{}".into())
}
