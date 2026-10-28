//! A stand-in room supervisor for the tests of public rooms and of rooms a
//! connection opens: it creates, lists and deletes rooms as the real one
//! does, and can refuse, fail, or hold its answers.
use std::{
    sync::{Arc, Mutex},
    time::Duration,
};

use axum::{
    Router,
    body::Bytes,
    extract::{Path, State},
    http::{HeaderMap, StatusCode as AxumStatus},
    routing::post,
};
use serde_json::{Value as Json, json};
use tokio::sync::Notify;
use zeroize::Zeroizing;

use crate::{Config, config, integrations::Secrets};

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
    /// How long `POST /rooms` takes to answer, once any hold is released.
    pub create_delay: Duration,
    /// Holds the answer to `GET /rooms` until released. The list is read
    /// before the hold, so the answer is as of the moment it arrived.
    pub list_hold: Option<Arc<Gate>>,
    /// Rooms `DELETE /rooms/{id}` was asked to close, in order.
    pub deleted: Vec<String>,
    /// Fails `DELETE /rooms/{id}`.
    pub fail_delete: bool,
    /// `GET /rooms` requests received.
    pub listed: usize,
    /// Holds the answer to `POST /rooms` after the room is hosted and listed
    /// and before the creator is told, which is the window in which the
    /// bridge has not stored the invitation yet.
    pub after_create: Option<Arc<Gate>>,
    /// Lists each room with the `rev` the real supervisor adds. Off, the
    /// list is what an older supervisor answers. The rev moves when `report`,
    /// `report_details` or `touch` change the room; a test that edits
    /// `rooms` directly calls `touch`.
    pub revs: bool,
    /// The last rev handed out.
    pub rev: u64,
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
        let delay = fake.lock().unwrap().create_delay;
        if !delay.is_zero() {
            tokio::time::sleep(delay).await;
        }
        let room_id = body["room_id"].as_str().unwrap().to_owned();
        let after_create = {
            let mut fake = fake.lock().unwrap();
            if let Some(reason) = fake.refuse {
                return (
                    AxumStatus::CONFLICT,
                    json!({ "reason": reason }).to_string(),
                );
            }
            fake.created.push(body.clone());
            let rev = next_rev(&mut fake);
            fake.rooms.push(json!({
                "room_id": room_id, "members": 0, "capacity": body["capacity"],
                "tables_playing": 0, "invitation": format!("sf4e3:{room_id}"), "banned": [],
                "opened": false, "rev": rev,
            }));
            fake.after_create.clone()
        };
        if let Some(gate) = after_create {
            gate.arrived.notify_one();
            gate.release.notified().await;
        }
        (
            AxumStatus::CREATED,
            json!({ "invitation": format!("sf4e3:{room_id}"), "region": "use1" }).to_string(),
        )
    }
    async fn list(State(fake): State<Supervisor>, headers: HeaderMap) -> (AxumStatus, String) {
        let (answer, hold) = {
            let mut fake = fake.lock().unwrap();
            fake.listed += 1;
            if !authorized(&headers) || fake.down {
                return (AxumStatus::INTERNAL_SERVER_ERROR, "{}".into());
            }
            let mut rooms = fake.rooms.clone();
            if !fake.revs {
                for room in &mut rooms {
                    room.as_object_mut().map(|room| room.remove("rev"));
                }
            }
            (Json::Array(rooms).to_string(), fake.list_hold.clone())
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

fn next_rev(fake: &mut Fake) -> u64 {
    fake.rev += 1;
    fake.rev
}

/// Changes the room `room_id` as `edit` says and moves its rev, as the
/// supervisor does for a report that differs.
fn change(fake: &Supervisor, room_id: &str, edit: impl FnOnce(&mut Json)) {
    let mut fake = fake.lock().unwrap();
    let rev = next_rev(&mut fake);
    let room = fake
        .rooms
        .iter_mut()
        .find(|room| room["room_id"] == room_id)
        .expect("the supervisor hosts the room");
    edit(room);
    room["rev"] = json!(rev);
}

/// Moves the rev of `room_id` after a test edited the room in `rooms` itself.
pub fn touch(fake: &Supervisor, room_id: &str) {
    change(fake, room_id, |_| {});
}

/// What the stand-in reports for `room_id`.
pub fn report(fake: &Supervisor, room_id: &str, members: u32, tables: u32, banned: &[&str]) {
    change(fake, room_id, |room| {
        room["members"] = json!(members);
        // The supervisor latches the first member it sees.
        room["opened"] = json!(room["opened"] == true || members >= 1);
        room["tables_playing"] = json!(tables);
        room["banned"] = json!(banned);
    });
}

/// What the stand-in reports as the room host's `details` for `room_id`, as
/// the supervisor would pass it on: any JSON value.
pub fn report_details(fake: &Supervisor, room_id: &str, details: Json) {
    change(fake, room_id, |room| room["details"] = details);
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
        skill_badges: false,
        developers: Vec::new(),
    });
    config.integration_secrets = Some("unused-in-tests.json".into());
}
