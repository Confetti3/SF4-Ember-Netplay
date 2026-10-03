//! A stand-in room supervisor for the tests of public rooms and of rooms a
//! connection opens: it creates, lists and deletes rooms as the real one
//! does, and can refuse, fail, or hold its answers.
use std::sync::{Arc, Mutex};

use axum::{
    Router,
    body::Bytes,
    extract::{Path, State},
    http::{HeaderMap, StatusCode as AxumStatus},
    routing::post,
};
use ember_bridge::{Config, config, integrations::Secrets};
use serde_json::{Value as Json, json};
use tokio::sync::Notify;
use zeroize::Zeroizing;

pub const SECRET: &str = "test-secret";
pub const BUILD: &str = "build-1";
pub const ENDPOINT: &str = "ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12";

/// What the stand-in supervisor holds and how it answers.
#[derive(Default)]
pub struct Fake {
    /// `POST /rooms` bodies, as received.
    pub created: Vec<Json>,
    /// What `GET /rooms` lists.
    pub rooms: Vec<Json>,
    /// Refuses `POST /rooms` with this reason.
    pub refuse: Option<&'static str>,
    /// Fails `GET /rooms`.
    pub down: bool,
    /// Holds `POST /rooms` until released.
    pub hold: Option<Arc<Gate>>,
    /// Holds the answer to `GET /rooms` until released. The list is read
    /// before the hold, so the answer is as of the moment it arrived.
    pub list_hold: Option<Arc<Gate>>,
    /// Rooms `DELETE /rooms/{id}` was asked to close, in order.
    pub deleted: Vec<String>,
    /// Fails `DELETE /rooms/{id}`.
    pub fail_delete: bool,
}

#[derive(Default)]
pub struct Gate {
    pub arrived: Notify,
    pub release: Notify,
}

pub type Supervisor = Arc<Mutex<Fake>>;

fn authorized(headers: &HeaderMap) -> bool {
    headers
        .get("authorization")
        .and_then(|value| value.to_str().ok())
        == Some(&format!("Bearer {SECRET}"))
}

pub async fn fake_supervisor() -> (Supervisor, String) {
    async fn create(
        State(fake): State<Supervisor>,
        headers: HeaderMap,
        body: Bytes,
    ) -> (AxumStatus, String) {
        if !authorized(&headers) {
            return (AxumStatus::UNAUTHORIZED, "{}".into());
        }
        let body: Json = serde_json::from_slice(&body).unwrap();
        let hold = fake.lock().unwrap().hold.clone();
        if let Some(gate) = hold {
            gate.arrived.notify_one();
            gate.release.notified().await;
        }
        let mut fake = fake.lock().unwrap();
        if let Some(reason) = fake.refuse {
            return (
                AxumStatus::CONFLICT,
                json!({ "reason": reason }).to_string(),
            );
        }
        let room_id = body["room_id"].as_str().unwrap().to_owned();
        fake.created.push(body.clone());
        fake.rooms.push(json!({
            "room_id": room_id, "members": 0, "capacity": body["capacity"],
            "tables_playing": 0, "invitation": format!("sf4e3:{room_id}"), "banned": [],
            "opened": false,
        }));
        (
            AxumStatus::CREATED,
            json!({ "invitation": format!("sf4e3:{room_id}"), "region": "use1" }).to_string(),
        )
    }
    async fn list(State(fake): State<Supervisor>, headers: HeaderMap) -> (AxumStatus, String) {
        let (answer, hold) = {
            let fake = fake.lock().unwrap();
            if !authorized(&headers) || fake.down {
                return (AxumStatus::INTERNAL_SERVER_ERROR, "{}".into());
            }
            (
                Json::Array(fake.rooms.clone()).to_string(),
                fake.list_hold.clone(),
            )
        };
        if let Some(gate) = hold {
            gate.arrived.notify_one();
            gate.release.notified().await;
        }
        (AxumStatus::OK, answer)
    }
    async fn delete(
        State(fake): State<Supervisor>,
        headers: HeaderMap,
        Path(id): Path<String>,
    ) -> AxumStatus {
        if !authorized(&headers) {
            return AxumStatus::UNAUTHORIZED;
        }
        let mut fake = fake.lock().unwrap();
        if fake.fail_delete {
            return AxumStatus::INTERNAL_SERVER_ERROR;
        }
        fake.rooms.retain(|room| room["room_id"] != id.as_str());
        fake.deleted.push(id);
        AxumStatus::NO_CONTENT
    }
    let fake = Supervisor::default();
    let app = Router::new()
        .route("/rooms", post(create).get(list))
        .route("/rooms/{id}", axum::routing::delete(delete))
        .with_state(fake.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let url = format!("http://{}", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await });
    (fake, url)
}

/// What the stand-in reports for `room_id`.
pub fn report(fake: &Supervisor, room_id: &str, members: u32, tables: u32, banned: &[&str]) {
    let mut fake = fake.lock().unwrap();
    let room = fake
        .rooms
        .iter_mut()
        .find(|room| room["room_id"] == room_id)
        .expect("the supervisor hosts the room");
    room["members"] = json!(members);
    // The supervisor latches the first member it sees.
    room["opened"] = json!(room["opened"] == true || members >= 1);
    room["tables_playing"] = json!(tables);
    room["banned"] = json!(banned);
}

/// The integration secrets of a bridge whose supervisor is this one.
pub fn secrets() -> Secrets {
    Secrets {
        rooms_supervisor_secret: Some(Zeroizing::new(SECRET.into())),
        ..Default::default()
    }
}

/// Points `config` at the supervisor at `url`.
pub fn enable(config: &mut Config, url: String) {
    config.rooms = Some(config::Rooms {
        supervisor_url: url,
    });
    config.integration_secrets = Some("unused-in-tests.json".into());
}
