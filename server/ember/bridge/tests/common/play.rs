//! A match played through the bridge as two fighters' helpers would: claims,
//! the room and its binding, game permits and signed reports. Shared by the
//! play tests and the platform tests that need a set played for real.
use ember_protocol::{
    PublicKey,
    challenge::{Action, Method},
    encoding::Counter,
    play::{Observation, SignedBinding, SignedPermit},
    report::Outcome,
};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};

use super::{Bridge, Player};

pub const BUILD: &str = "build-1";
pub const ROOM: &str = "0123456789abcdef0123456789abcdef";

/// A match's two fighters, P1 (`a`) and P2 (`b`), on their bridge.
pub trait Table {
    fn bridge(&self) -> &Bridge;
    fn a(&self) -> &Player;
    fn b(&self) -> &Player;
}

/// A stand-in Iroh endpoint ID, distinct per player.
pub fn endpoint(player: &Player) -> String {
    player.id().as_str().as_bytes()[5..37]
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

pub async fn act(
    f: &impl Table,
    player: &Player,
    action: Action,
    path: &str,
    command: Json,
) -> (StatusCode, Json) {
    let body = f
        .bridge()
        .prove(player, action, Method::Post, path, command)
        .await;
    f.bridge()
        .send_proof(player, Method::Post, path, body)
        .await
}

/// A claim from a helper that does not say whether its fighter is ready.
pub async fn claim(
    f: &impl Table,
    player: &Player,
    id: &str,
    endpoint: &str,
    build: &str,
) -> (StatusCode, Json) {
    claim_with(
        f,
        player,
        id,
        json!({ "endpoint_id": endpoint, "build_id": build }),
    )
    .await
}

/// A claim saying whether the fighter has pressed Ready at the table.
pub async fn claim_ready(
    f: &impl Table,
    player: &Player,
    id: &str,
    ready: bool,
) -> (StatusCode, Json) {
    claim_with(
        f,
        player,
        id,
        json!({ "endpoint_id": endpoint(player), "build_id": BUILD, "ready": ready }),
    )
    .await
}

async fn claim_with(
    f: &impl Table,
    player: &Player,
    id: &str,
    mut command: Json,
) -> (StatusCode, Json) {
    command["helper_instance_id"] = json!("ins_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a01");
    act(
        f,
        player,
        Action::MatchClaim,
        &format!("/v1/matches/{id}/claims"),
        command,
    )
    .await
}

pub async fn publish(
    f: &impl Table,
    player: &Player,
    id: &str,
    room: &str,
    lease: Option<&Json>,
    replaces: Option<&str>,
) -> (StatusCode, Json) {
    act(
        f,
        player,
        Action::RoomPublish,
        &format!("/v1/matches/{id}/room"),
        json!({
            "room_id": room,
            "invitation": format!("sf4e3:{room}"),
            "lease_id": lease.map(|answer| answer["lease_id"].clone()),
            "fence": lease.map(|answer| answer["fence"].clone()),
            "replaces": replaces,
        }),
    )
    .await
}

pub async fn bridge_key(f: &impl Table) -> (PublicKey, String) {
    let response = f
        .bridge()
        .client
        .get(f.bridge().url("/v1/signing-keys"))
        .send()
        .await
        .unwrap();
    let (_, keys) = super::read(response).await;
    let key = &keys["keys"][0];
    (
        PublicKey::from_b64u(key["x"].as_str().unwrap()).unwrap(),
        key["kid"].as_str().unwrap().to_owned(),
    )
}

pub fn descriptor(binding: &Json, player: &Player, generation: u64) -> Json {
    json!({
        "assignment_generation": binding["assignment_generation"],
        "binding_revision": binding["binding_revision"],
        "room_id": binding["room_id"],
        "table_id": 0,
        "match_generation": generation.to_string(),
        "rules_digest": binding["rules_digest"],
        "roster_digest": binding["roster_digest"],
        "build_id": binding["build_id"],
        "endpoint_id": endpoint(player),
    })
}

pub async fn prepare(
    f: &impl Table,
    player: &Player,
    id: &str,
    binding: &Json,
    generation: u64,
) -> (StatusCode, Json) {
    act(
        f,
        player,
        Action::AttemptPrepare,
        &format!("/v1/matches/{id}/attempts/prepare"),
        descriptor(binding, player, generation),
    )
    .await
}

/// Both fighters prepare the same game; returns the permit both receive.
pub async fn permit(f: &impl Table, id: &str, binding: &Json, generation: u64) -> SignedPermit {
    let (status, first) = prepare(f, f.a(), id, binding, generation).await;
    assert_eq!(
        (status, first["state"].as_str()),
        (StatusCode::OK, Some("pending")),
        "{first}"
    );
    let (status, second) = prepare(f, f.b(), id, binding, generation).await;
    assert_eq!(
        (status, second["state"].as_str()),
        (StatusCode::OK, Some("permitted")),
        "{second}"
    );
    // The first fighter asking again gets the same permit.
    let (_, again) = prepare(f, f.a(), id, binding, generation).await;
    assert_eq!(again["permit"], second["permit"]);
    let signed: SignedPermit = serde_json::from_value(second["permit"].clone()).unwrap();
    let (key, kid) = bridge_key(f).await;
    signed.verify(&key, &kid).unwrap();
    signed
}

pub fn observation(n: u8, result: Outcome) -> Observation {
    let native = result.is_native_result();
    Observation {
        observation_id: format!("obs_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a{n:02}"),
        result,
        capture_frame: native.then_some(Counter(900)),
        confirmed_input_frame: native.then_some(Counter(899)),
        observed_at: 1,
        helper_instance_id: "ins_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a01".into(),
    }
}

pub async fn report(
    f: &impl Table,
    sender: &Player,
    reporter: &Player,
    id: &str,
    permit: &SignedPermit,
    n: u8,
    result: Outcome,
) -> (StatusCode, Json) {
    let signed = permit
        .permit
        .report(reporter.id(), observation(n, result))
        .unwrap()
        .sign(&reporter.identity)
        .unwrap();
    let response = f
        .bridge()
        .client
        .post(f.bridge().url(&format!("/v1/matches/{id}/reports")))
        .bearer_auth(sender.token())
        .header("content-type", "application/json")
        .body(serde_json::to_vec(&signed).unwrap())
        .send()
        .await
        .unwrap();
    super::read(response).await
}

/// Claims and publishes the room; returns the binding JSON both fighters hold.
pub async fn bound(f: &impl Table, id: &str) -> Json {
    let (status, host) = claim(f, f.a(), id, &endpoint(f.a()), BUILD).await;
    assert_eq!(
        (status, host["role"].as_str()),
        (StatusCode::OK, Some("host")),
        "{host}"
    );
    let (status, wait) = claim(f, f.b(), id, &endpoint(f.b()), BUILD).await;
    assert_eq!(
        (status, wait["role"].as_str()),
        (StatusCode::OK, Some("wait")),
        "{wait}"
    );
    // Only the lease holder publishes the first room.
    let (status, refused) = publish(f, f.b(), id, ROOM, Some(&host), None).await;
    assert_eq!(status, StatusCode::CONFLICT, "{refused}");
    let (status, room) = publish(f, f.a(), id, ROOM, Some(&host), None).await;
    assert_eq!(
        (status, room["role"].as_str()),
        (StatusCode::OK, Some("room")),
        "{room}"
    );
    let (_, joined) = claim(f, f.b(), id, &endpoint(f.b()), BUILD).await;
    assert_eq!(joined["invitation"], format!("sf4e3:{ROOM}"));
    let signed: SignedBinding = serde_json::from_value(joined["binding"].clone()).unwrap();
    let (key, kid) = bridge_key(f).await;
    let binding = signed.verify(&key, &kid).unwrap();
    assert_eq!(binding.fighters[0].endpoint_id, endpoint(f.a()));
    assert_eq!(binding.slot_of(&endpoint(f.b())), Some(1));
    assert_eq!(binding.games_to_win, 2);
    serde_json::to_value(binding).unwrap()
}

/// Game `n`'s replay, as the game archives it, saved `n` seconds after the
/// set's first.
pub fn replay(n: u8) -> Vec<u8> {
    ember_protocol::replays::wrap(&[n; 64], 1_800_000_000 + u32::from(n))
}

/// `sender` sends a game's replay, played under room revision `revision`.
pub async fn send_replay(
    f: &impl Table,
    sender: &Player,
    id: &str,
    replay: &[u8],
    revision: u64,
) -> StatusCode {
    let crc = ember_protocol::replays::crc_name(replay).unwrap();
    f.bridge()
        .client
        .put(f.bridge().url(&format!(
            "/v1/matches/{id}/replays/{crc}?revision={revision}"
        )))
        .bearer_auth(sender.token())
        .body(replay.to_vec())
        .send()
        .await
        .unwrap()
        .status()
}

/// The set's pack, read as `token`: its header and each game's bytes.
pub async fn pack(f: &impl Table, token: &str, id: &str) -> (Json, Vec<Vec<u8>>) {
    let response = f
        .bridge()
        .client
        .get(f.bridge().url(&format!("/v1/matches/{id}/replays.pack")))
        .bearer_auth(token)
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let bytes = response.bytes().await.unwrap();
    assert_eq!(&bytes[..8], ember_protocol::replays::PACK_MAGIC);
    let length = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
    let header: Json = serde_json::from_slice(&bytes[12..12 + length]).unwrap();
    let mut at = 12 + length;
    let games = header["games"]
        .as_array()
        .unwrap()
        .iter()
        .map(|game| {
            let size = game["size"].as_u64().unwrap() as usize;
            at += size;
            bytes[at - size..at].to_vec()
        })
        .collect();
    assert_eq!(at, bytes.len());
    (header, games)
}
