//! Public rooms against a stand-in room supervisor: the routes are off without
//! one, a room is listed once the supervisor reports a member, tickets verify
//! against the bridge's published key, and the supervisor's reports (members,
//! kicks, rooms gone) move the bridge's view of each room.
mod common;

use std::sync::{Arc, Mutex};

use axum::{
    Router,
    body::Bytes,
    extract::{Path, State},
    http::{HeaderMap, StatusCode as AxumStatus},
    routing::post,
};
use common::{Bridge, Player, code};
use ember_bridge::{config, integrations::Secrets};
use ember_protocol::{
    PublicKey, SigningIdentity,
    rooms::{MAX_ROOM_BANS, RoomAdmission, RoomList, RoomSummary, TICKET_SECS},
};
use reqwest::StatusCode;
use serde_json::{Value as Json_, json};
use tokio::sync::Notify;
use zeroize::Zeroizing;

const SECRET: &str = "test-secret";
const BUILD: &str = "build-1";
const ENDPOINT: &str = "ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12ab12";

/// What the stand-in supervisor holds and how it answers.
#[derive(Default)]
struct Fake {
    /// `POST /rooms` bodies, as received.
    created: Vec<Json_>,
    /// What `GET /rooms` lists.
    rooms: Vec<Json_>,
    /// Refuses `POST /rooms` with this reason.
    refuse: Option<&'static str>,
    /// Fails `GET /rooms`.
    down: bool,
    /// Holds `POST /rooms` until released.
    hold: Option<Arc<Gate>>,
    /// Holds the answer to `GET /rooms` until released. The list is read
    /// before the hold, so the answer is as of the moment it arrived.
    list_hold: Option<Arc<Gate>>,
    deleted: Vec<String>,
}

#[derive(Default)]
struct Gate {
    arrived: Notify,
    release: Notify,
}

type Supervisor = Arc<Mutex<Fake>>;

fn authorized(headers: &HeaderMap) -> bool {
    headers
        .get("authorization")
        .and_then(|value| value.to_str().ok())
        == Some(&format!("Bearer {SECRET}"))
}

async fn fake_supervisor() -> (Supervisor, String) {
    async fn create(
        State(fake): State<Supervisor>,
        headers: HeaderMap,
        body: Bytes,
    ) -> (AxumStatus, String) {
        if !authorized(&headers) {
            return (AxumStatus::UNAUTHORIZED, "{}".into());
        }
        let body: Json_ = serde_json::from_slice(&body).unwrap();
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
                Json_::Array(fake.rooms.clone()).to_string(),
                fake.list_hold.clone(),
            )
        };
        if let Some(gate) = hold {
            gate.arrived.notify_one();
            gate.release.notified().await;
        }
        (AxumStatus::OK, answer)
    }
    async fn delete(State(fake): State<Supervisor>, Path(id): Path<String>) -> AxumStatus {
        let mut fake = fake.lock().unwrap();
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
fn report(fake: &Supervisor, room_id: &str, members: u32, tables: u32, banned: &[&str]) {
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

fn secrets() -> Secrets {
    Secrets {
        rooms_supervisor_secret: Some(Zeroizing::new(SECRET.into())),
        ..Default::default()
    }
}

async fn start() -> (Bridge, Supervisor) {
    let (fake, url) = fake_supervisor().await;
    let bridge = Bridge::start_with(
        |config| {
            config.rooms = Some(config::Rooms {
                supervisor_url: url,
            });
            config.integration_secrets = Some("unused-in-tests.json".into());
        },
        secrets(),
    )
    .await;
    (bridge, fake)
}

async fn player(bridge: &Bridge, byte: u8) -> Player {
    let mut player = bridge.player(byte);
    bridge.open_session(&mut player).await;
    player
}

/// A request as nginx would pass it on: with the client's address.
async fn send(
    bridge: &Bridge,
    method: reqwest::Method,
    token: &str,
    ip: &str,
    path: &str,
    body: Option<Json_>,
) -> (StatusCode, Json_) {
    let mut request = bridge
        .client
        .request(method, bridge.url(path))
        .bearer_auth(token)
        .header("x-real-ip", ip);
    if let Some(body) = body {
        request = request
            .header("content-type", "application/json")
            .body(serde_json::to_vec(&body).unwrap());
    }
    common::read(request.send().await.unwrap()).await
}

async fn create_room(
    bridge: &Bridge,
    player: &Player,
    ip: &str,
    name: &str,
    capacity: u8,
) -> (StatusCode, Json_) {
    send(
        bridge,
        reqwest::Method::POST,
        player.token(),
        ip,
        "/v1/rooms",
        Some(json!({ "name": name, "capacity": capacity, "build_id": BUILD })),
    )
    .await
}

async fn ticket(
    bridge: &Bridge,
    player: &Player,
    ip: &str,
    room_id: &str,
    build: &str,
) -> (StatusCode, Json_) {
    send(
        bridge,
        reqwest::Method::POST,
        player.token(),
        ip,
        &format!("/v1/rooms/{room_id}/tickets"),
        Some(json!({ "endpoint_id": ENDPOINT, "build_id": build })),
    )
    .await
}

async fn listing(bridge: &Bridge, player: &Player, query: &str) -> Vec<RoomSummary> {
    let (status, body) = send(
        bridge,
        reqwest::Method::GET,
        player.token(),
        "198.51.100.1",
        &format!("/v1/rooms{query}"),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{body}");
    serde_json::from_value::<RoomList>(body).unwrap().rooms
}

/// Creates a room as `creator` from its own address.
async fn open_room(bridge: &Bridge, creator: &Player, ip: &str, capacity: u8) -> String {
    let (status, room) = create_room(bridge, creator, ip, "Friendly matches", capacity).await;
    assert_eq!(status, StatusCode::CREATED, "{room}");
    room["room_id"].as_str().unwrap().to_owned()
}

fn reason(body: &Json_) -> &str {
    body["error"]["details"]["reason"].as_str().unwrap_or("")
}

#[tokio::test]
async fn a_bridge_without_a_supervisor_offers_no_rooms() {
    let bridge = Bridge::start().await;
    let kate = player(&bridge, 1).await;
    let (status, capabilities) = bridge.get(kate.token(), "/v1/capabilities").await;
    assert_eq!(status, StatusCode::OK);
    assert!(!capabilities["features"].to_string().contains("rooms"));
    let (status, body) = send(
        &bridge,
        reqwest::Method::GET,
        kate.token(),
        "198.51.100.1",
        "/v1/rooms",
        None,
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND, "{body}");
    let (status, _) = create_room(&bridge, &kate, "198.51.100.1", "Room", 4).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = ticket(&bridge, &kate, "198.51.100.1", &"a".repeat(32), BUILD).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    // A supervisor address without its secret is as good as none.
    let (_, url) = fake_supervisor().await;
    let bridge = Bridge::start_with(
        |config| {
            config.rooms = Some(config::Rooms {
                supervisor_url: url,
            });
            config.integration_secrets = Some("unused-in-tests.json".into());
        },
        Default::default(),
    )
    .await;
    let kate = player(&bridge, 1).await;
    let (_, capabilities) = bridge.get(kate.token(), "/v1/capabilities").await;
    assert!(!capabilities["features"].to_string().contains("rooms"));
}

#[tokio::test]
async fn the_supervisor_must_be_a_loopback_address() {
    let mut config = (*Bridge::start().await.state().config).clone();
    config.integration_secrets = Some("integrations.json".into());
    for refused in [
        "https://127.0.0.1:8790",
        "http://rooms.example:8790",
        "http://10.0.0.5:8790",
        "http://127.0.0.1:8790/rooms",
        "http://user:pw@127.0.0.1:8790",
    ] {
        config.rooms = Some(config::Rooms {
            supervisor_url: refused.into(),
        });
        assert!(config.validate().is_err(), "{refused}");
    }
    for accepted in [
        "http://127.0.0.1:8790",
        "http://localhost:8790/",
        "http://[::1]:8790",
    ] {
        config.rooms = Some(config::Rooms {
            supervisor_url: accepted.into(),
        });
        assert!(config.validate().is_ok(), "{accepted}");
    }
}

#[tokio::test]
async fn a_player_session_is_needed() {
    let (bridge, _) = start().await;
    let (_, capabilities) = bridge
        .get(&bridge.provider("mock-a").await, "/v1/capabilities")
        .await;
    assert!(capabilities["features"].to_string().contains("rooms"));
    let response = bridge
        .client
        .get(bridge.url("/v1/rooms"))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::UNAUTHORIZED);
    let provider = bridge.provider("mock-a").await;
    let (status, _) = bridge.get(&provider, "/v1/rooms").await;
    assert_eq!(status, StatusCode::FORBIDDEN);
}

#[tokio::test]
async fn creating_a_room_asks_the_supervisor_with_the_bridges_ticket_key() {
    let (bridge, fake) = start().await;
    let kate = player(&bridge, 1).await;
    let (status, room) = create_room(&bridge, &kate, "198.51.100.1", "Friendly matches", 8).await;
    assert_eq!(status, StatusCode::CREATED, "{room}");
    let summary: RoomSummary = serde_json::from_value(room).unwrap();
    summary.check().unwrap();
    assert_eq!(
        (summary.name.as_str(), summary.capacity, summary.members),
        ("Friendly matches", 8, 0)
    );
    assert_eq!(summary.region, "use1");

    let (status, keys) = bridge.get(kate.token(), "/v1/signing-keys").await;
    assert_eq!(status, StatusCode::OK);
    let sent = fake.lock().unwrap().created[0].clone();
    assert_eq!(sent["room_id"], summary.room_id.as_str());
    assert_eq!(sent["creator"], kate.id().as_str());
    assert_eq!(sent["bridge_id"], bridge.bridge_id.as_str());
    assert_eq!(sent["build_id"], BUILD);
    assert_eq!(sent["ticket_key"], keys["keys"][0]["x"]);
    assert_eq!(sent["ticket_kid"], keys["keys"][0]["kid"]);
}

#[tokio::test]
async fn names_and_sizes_are_checked_before_the_supervisor_is_asked() {
    let (bridge, fake) = start().await;
    let kate = player(&bridge, 1).await;
    for name in ["", " padded", "two\nlines", &"x".repeat(65)] {
        let (status, body) = create_room(&bridge, &kate, "198.51.100.1", name, 4).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{name:?}");
        assert_eq!(reason(&body), "invalid_name");
    }
    let (status, body) = create_room(&bridge, &kate, "198.51.100.1", "Room", 1).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    assert_eq!(code(&body), "invalid_request");
    assert!(fake.lock().unwrap().created.is_empty());
    // None of that used up the creator's one room.
    open_room(&bridge, &kate, "198.51.100.1", 4).await;
}

#[tokio::test]
async fn one_open_room_per_creator_and_two_per_address() {
    let (bridge, fake) = start().await;
    let (kate, sam, kim, lee) = (
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
        player(&bridge, 4).await,
    );
    let first = open_room(&bridge, &kate, "198.51.100.1", 4).await;
    let (status, body) = create_room(&bridge, &kate, "198.51.100.9", "Again", 4).await;
    assert_eq!(status, StatusCode::CONFLICT, "{body}");
    assert_eq!(reason(&body), "room_limit");

    // Two creators behind one address fill it; the third is turned away.
    open_room(&bridge, &sam, "198.51.100.2", 4).await;
    open_room(&bridge, &kim, "198.51.100.2", 4).await;
    let (status, body) = create_room(&bridge, &lee, "198.51.100.2", "Third", 4).await;
    assert_eq!(status, StatusCode::CONFLICT, "{body}");
    assert_eq!(reason(&body), "room_limit");
    // An IPv6 address counts by its /64.
    open_room(&bridge, &lee, "2001:db8:1:2::1", 4).await;
    assert_eq!(fake.lock().unwrap().created.len(), 4);

    // Once the supervisor stops reporting Kate's room she may open another.
    fake.lock()
        .unwrap()
        .rooms
        .retain(|room| room["room_id"] != first.as_str());
    bridge.clock.advance(2);
    ember_bridge::poll_rooms(bridge.state()).await;
    open_room(&bridge, &kate, "198.51.100.1", 4).await;
}

#[tokio::test]
async fn the_address_limit_survives_a_bridge_restart() {
    let (mut bridge, fake) = start().await;
    let (sam, kim, lee) = (
        player(&bridge, 2).await,
        player(&bridge, 3).await,
        player(&bridge, 4).await,
    );
    open_room(&bridge, &sam, "198.51.100.2", 4).await;
    open_room(&bridge, &kim, "198.51.100.2", 4).await;

    // The supervisor keeps both rooms alive across the restart; the stored
    // address hashes must still match the same address afterwards.
    bridge.restart(secrets()).await;
    assert_eq!(fake.lock().unwrap().rooms.len(), 2);
    let (status, body) = create_room(&bridge, &lee, "198.51.100.2", "Third", 4).await;
    assert_eq!(status, StatusCode::CONFLICT, "{body}");
    assert_eq!(reason(&body), "room_limit");
    assert_eq!(fake.lock().unwrap().created.len(), 2);
    // Another address is still free.
    open_room(&bridge, &lee, "198.51.100.3", 4).await;
}

#[tokio::test]
async fn the_supervisors_refusals_reach_the_player_and_free_the_creator() {
    let (bridge, fake) = start().await;
    let kate = player(&bridge, 1).await;
    for (refusal, status) in [
        ("unsupported_build", StatusCode::UNPROCESSABLE_ENTITY),
        ("room_limit", StatusCode::CONFLICT),
    ] {
        fake.lock().unwrap().refuse = Some(refusal);
        let (got, body) = create_room(&bridge, &kate, "198.51.100.1", "Room", 4).await;
        assert_eq!(got, status, "{body}");
        assert_eq!(reason(&body), refusal);
    }
    fake.lock().unwrap().refuse = None;
    open_room(&bridge, &kate, "198.51.100.1", 4).await;
}

#[tokio::test]
async fn a_room_is_listed_once_it_has_a_member_and_fullest_last() {
    let (bridge, fake) = start().await;
    let (kate, sam, kim, lee) = (
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
        player(&bridge, 4).await,
    );
    let almost = open_room(&bridge, &kate, "198.51.100.1", 4).await;
    let quiet = open_room(&bridge, &sam, "198.51.100.2", 4).await;
    let big = open_room(&bridge, &kim, "198.51.100.3", 8).await;
    ember_bridge::poll_rooms(bridge.state()).await;
    assert!(listing(&bridge, &lee, "").await.is_empty());

    report(&fake, &almost, 3, 1, &[]);
    report(&fake, &quiet, 1, 0, &[]);
    report(&fake, &big, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let rooms = listing(&bridge, &lee, "").await;
    let order: Vec<_> = rooms.iter().map(|room| room.room_id.as_str()).collect();
    assert_eq!(order, [big.as_str(), quiet.as_str(), almost.as_str()]);
    assert_eq!(
        (rooms[2].members, rooms[2].tables_playing, rooms[2].capacity),
        (3, 1, 4)
    );
    assert_eq!(listing(&bridge, &lee, "?build_id=build-1").await.len(), 3);
    assert!(listing(&bridge, &lee, "?build_id=build-2").await.is_empty());

    // Everyone leaves: the room is no longer listed, though it is still open.
    report(&fake, &big, 0, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(listing(&bridge, &lee, "").await.len(), 2);
}

#[tokio::test]
async fn a_ticket_verifies_with_the_published_key_and_admits_its_endpoint() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 4).await;
    report(&fake, &room_id, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;

    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    let admission: RoomAdmission = serde_json::from_value(body).unwrap();
    admission.check().unwrap();
    assert_eq!(admission.invitation, format!("sf4e3:{room_id}"));
    assert_eq!(admission.room.members, 1);

    let (_, keys) = bridge.get(sam.token(), "/v1/signing-keys").await;
    let key = PublicKey::from_b64u(keys["keys"][0]["x"].as_str().unwrap()).unwrap();
    let kid = keys["keys"][0]["kid"].as_str().unwrap();
    let verified = admission.ticket.verify(&key, kid).unwrap();
    let now = bridge.clock.now();
    assert_eq!(&verified.ember_id, sam.id());
    assert_eq!(verified.bridge_id, bridge.bridge_id);
    assert_eq!(verified.expires_at, verified.issued_at + TICKET_SECS);
    assert!(verified.admits(&room_id, ENDPOINT, now));
    assert!(!verified.admits(&room_id, &"c".repeat(64), now));
    assert!(!verified.admits(&"c".repeat(32), ENDPOINT, now));
    assert!(!verified.admits(&room_id, ENDPOINT, now + TICKET_SECS + 1));

    // The creator takes a ticket the same way.
    let (status, _) = ticket(&bridge, &kate, "198.51.100.1", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED);
}

#[tokio::test]
async fn only_the_creator_gets_a_ticket_before_the_room_has_a_member() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 4).await;

    // The room is not public yet: another account is refused as if it were
    // not there, before and after a poll that reports no member.
    for _ in 0..2 {
        let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
        assert_eq!(status, StatusCode::NOT_FOUND, "{body}");
        assert_eq!(reason(&body), "room_not_open");
        ember_bridge::poll_rooms(bridge.state()).await;
    }
    // The creator is admitted, as is anyone once the supervisor has reported a
    // member.
    let (status, body) = ticket(&bridge, &kate, "198.51.100.1", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    report(&fake, &room_id, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");

    // The room empties: it is not listed, but being first is over for good.
    report(&fake, &room_id, 0, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert!(listing(&bridge, &sam, "").await.is_empty());
    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    let (status, _) = ticket(&bridge, &kate, "198.51.100.1", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED);
    // A room that does not exist is still not found.
    let (_, body) = ticket(&bridge, &sam, "198.51.100.2", &"e".repeat(32), BUILD).await;
    assert_eq!(reason(&body), "room_not_found");
}

/// The supervisor keeps only the latest status, so a creator who came and went
/// between two polls must still count: the room stays empty for the listing
/// but is no longer creator-only.
#[tokio::test]
async fn a_member_who_came_and_went_between_polls_still_opens_the_room() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 4).await;
    ember_bridge::poll_rooms(bridge.state()).await;
    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::NOT_FOUND, "{body}");

    // Occupied, then empty again, before the bridge looks.
    report(&fake, &room_id, 1, 0, &[]);
    report(&fake, &room_id, 0, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert!(listing(&bridge, &sam, "").await.is_empty());
    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
}

#[tokio::test]
async fn tickets_are_refused_for_the_wrong_build_a_full_room_and_a_banned_player() {
    let (bridge, fake) = start().await;
    let (kate, sam, kim) = (
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
    );
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 2).await;
    report(&fake, &room_id, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;

    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, "build-2").await;
    assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY, "{body}");
    assert_eq!(reason(&body), "unsupported_build");
    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &"e".repeat(32), BUILD).await;
    assert_eq!(status, StatusCode::NOT_FOUND, "{body}");
    assert_eq!(reason(&body), "room_not_found");

    // The host kicks Sam; Kim still may join.
    report(&fake, &room_id, 1, 0, &[sam.id().as_str()]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{body}");
    assert_eq!(reason(&body), "banned");
    // The ban stays after the supervisor stops listing it.
    report(&fake, &room_id, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let (status, _) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _) = ticket(&bridge, &kim, "198.51.100.3", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED);

    report(&fake, &room_id, 2, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let (status, body) = ticket(&bridge, &kim, "198.51.100.3", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CONFLICT, "{body}");
    assert_eq!(reason(&body), "room_full");
}

#[tokio::test]
async fn a_room_the_supervisor_stops_reporting_closes() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 4).await;
    report(&fake, &room_id, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(listing(&bridge, &sam, "").await.len(), 1);

    // An unreachable supervisor says nothing about the room.
    fake.lock().unwrap().down = true;
    bridge.clock.advance(2);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(listing(&bridge, &sam, "").await.len(), 1);
    let (status, _) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED);

    {
        let mut hosting = fake.lock().unwrap();
        hosting.down = false;
        hosting.rooms.clear();
    }
    ember_bridge::poll_rooms(bridge.state()).await;
    assert!(listing(&bridge, &sam, "").await.is_empty());
    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::NOT_FOUND, "{body}");
    assert_eq!(reason(&body), "room_not_found");
}

#[tokio::test]
async fn a_room_still_starting_is_not_closed_by_a_poll() {
    let (bridge, fake) = start().await;
    let kate = player(&bridge, 1).await;
    let gate = Arc::new(Gate::default());
    fake.lock().unwrap().hold = Some(gate.clone());
    let creating = {
        let (client, url, token) = (
            bridge.client.clone(),
            bridge.url("/v1/rooms"),
            kate.token().to_owned(),
        );
        tokio::spawn(async move {
            client
                .post(url)
                .bearer_auth(token)
                .header("content-type", "application/json")
                .body(json!({ "name": "Slow", "capacity": 4, "build_id": BUILD }).to_string())
                .send()
                .await
                .unwrap()
        })
    };
    gate.arrived.notified().await;
    bridge.clock.advance(2);
    ember_bridge::poll_rooms(bridge.state()).await;
    // A second room by the same creator is refused while the first starts.
    let (status, _) = create_room(&bridge, &kate, "198.51.100.1", "Other", 4).await;
    assert_eq!(status, StatusCode::CONFLICT);
    gate.release.notify_one();
    let (status, room) = common::read(creating.await.unwrap()).await;
    assert_eq!(status, StatusCode::CREATED, "{room}");
    let room_id = room["room_id"].as_str().unwrap();
    report(&fake, room_id, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(listing(&bridge, &kate, "").await.len(), 1);
}

#[tokio::test]
async fn a_room_published_while_the_list_is_in_flight_is_not_closed() {
    let (bridge, fake) = start().await;
    let kate = player(&bridge, 1).await;
    let create_gate = Arc::new(Gate::default());
    fake.lock().unwrap().hold = Some(create_gate.clone());
    let creating = {
        let (client, url, token) = (
            bridge.client.clone(),
            bridge.url("/v1/rooms"),
            kate.token().to_owned(),
        );
        tokio::spawn(async move {
            client
                .post(url)
                .bearer_auth(token)
                .header("content-type", "application/json")
                .body(json!({ "name": "Slow", "capacity": 4, "build_id": BUILD }).to_string())
                .send()
                .await
                .unwrap()
        })
    };
    create_gate.arrived.notified().await;
    // The reservation is older than the poll that follows.
    bridge.clock.advance(2);
    let list_gate = Arc::new(Gate::default());
    fake.lock().unwrap().list_hold = Some(list_gate.clone());
    // The poll asks the supervisor, which answers as it stands: no rooms.
    // Then the creation finishes and the answer reaches the bridge.
    let finishing = async {
        list_gate.arrived.notified().await;
        create_gate.release.notify_one();
        let (status, room) = common::read(creating.await.unwrap()).await;
        assert_eq!(status, StatusCode::CREATED, "{room}");
        list_gate.release.notify_one();
        room["room_id"].as_str().unwrap().to_owned()
    };
    let (_, room_id) = tokio::join!(ember_bridge::poll_rooms(bridge.state()), finishing);

    // The report did not include the room, which is no evidence it ended.
    let (status, body) = ticket(&bridge, &kate, "198.51.100.1", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    fake.lock().unwrap().list_hold = None;
    report(&fake, &room_id, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(listing(&bridge, &kate, "").await.len(), 1);
    let (status, _) = ticket(&bridge, &kate, "198.51.100.1", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED);
}

async fn stored_bans(bridge: &Bridge) -> i64 {
    bridge
        .state()
        .db
        .read(|tx| Ok(tx.query_row("SELECT COUNT(*) FROM room_bans", [], |row| row.get(0))?))
        .await
        .unwrap()
}

#[tokio::test]
async fn a_rooms_bans_up_to_the_cap_are_all_kept() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 4).await;
    let banned: Vec<String> = (0..MAX_ROOM_BANS)
        .map(|index| {
            let mut seed = [0u8; 32];
            seed[..8].copy_from_slice(&(index as u64 + 100).to_le_bytes());
            SigningIdentity::from_seed(&Zeroizing::new(seed))
                .unwrap()
                .ember_id()
                .to_string()
        })
        .collect();
    // The last of the 512 is Sam.
    let mut reported: Vec<&str> = banned.iter().map(String::as_str).collect();
    reported[MAX_ROOM_BANS - 1] = sam.id().as_str();
    report(&fake, &room_id, 1, 0, &reported);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(stored_bans(&bridge).await, MAX_ROOM_BANS as i64);
    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{body}");
    assert_eq!(reason(&body), "banned");
    // Reporting the same bans again, or none, forgets nothing.
    report(&fake, &room_id, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(stored_bans(&bridge).await, MAX_ROOM_BANS as i64);
    let (status, _) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
}

#[tokio::test]
async fn ticket_requests_are_limited_per_address() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 4).await;
    report(&fake, &room_id, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    for _ in 0..30 {
        let (status, _) = ticket(&bridge, &sam, "203.0.113.7", &room_id, BUILD).await;
        assert_eq!(status, StatusCode::CREATED);
    }
    let (status, body) = ticket(&bridge, &sam, "203.0.113.7", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::TOO_MANY_REQUESTS, "{body}");
    // Another address, even for the same player, is counted apart.
    let (status, _) = ticket(&bridge, &sam, "203.0.113.8", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED);
}
