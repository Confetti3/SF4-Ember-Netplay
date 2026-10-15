//! Requests to the public room routes as a player would send them, shared by
//! the public room tests.
use reqwest::StatusCode;
use serde_json::{Value as Json_, json};

use ember_protocol::rooms::{RoomList, RoomSummary};

use super::{
    Bridge, Player,
    supervisor::{BUILD, ENDPOINT, Supervisor, enable, fake_supervisor, secrets},
};

pub async fn start() -> (Bridge, Supervisor) {
    let (fake, url) = fake_supervisor().await;
    let bridge = Bridge::start_with(|config| enable(config, url), secrets()).await;
    (bridge, fake)
}

pub async fn player(bridge: &Bridge, byte: u8) -> Player {
    let mut player = bridge.player(byte);
    bridge.open_session(&mut player).await;
    player
}

/// A request as nginx would pass it on: with the client's address.
pub async fn send(
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
    super::read(request.send().await.unwrap()).await
}

pub async fn create_room(
    bridge: &Bridge,
    player: &Player,
    ip: &str,
    name: &str,
    capacity: u8,
) -> (StatusCode, Json_) {
    let body = json!({ "name": name, "capacity": capacity, "build_id": BUILD });
    create_room_with(bridge, player, ip, body).await
}

/// `POST /v1/rooms` with `body` as it is.
pub async fn create_room_with(
    bridge: &Bridge,
    player: &Player,
    ip: &str,
    body: Json_,
) -> (StatusCode, Json_) {
    send(
        bridge,
        reqwest::Method::POST,
        player.token(),
        ip,
        "/v1/rooms",
        Some(body),
    )
    .await
}

pub async fn ticket(
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

pub async fn listing(bridge: &Bridge, player: &Player, query: &str) -> Vec<RoomSummary> {
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

/// The listing as the bridge sent it, for what the typed form would hide.
pub async fn raw_listing(bridge: &Bridge, player: &Player, query: &str) -> Json_ {
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
    body
}

/// Creates a room as `creator` from its own address.
pub async fn open_room(bridge: &Bridge, creator: &Player, ip: &str, capacity: u8) -> String {
    let (status, room) = create_room(bridge, creator, ip, "Friendly matches", capacity).await;
    assert_eq!(status, StatusCode::CREATED, "{room}");
    room["room_id"].as_str().unwrap().to_owned()
}

pub fn reason(body: &Json_) -> &str {
    body["error"]["details"]["reason"].as_str().unwrap_or("")
}
