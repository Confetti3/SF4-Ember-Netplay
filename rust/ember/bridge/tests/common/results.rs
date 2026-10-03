//! Organizer-reported matches and their results' delivery: the helpers the
//! BluMint and partner tests share. `provider` and `organizer` are the
//! credentials of the match's connection and tenant.
use std::time::Duration;

use reqwest::StatusCode;
use serde_json::{Value as Json, json};

use super::Bridge;

/// The organizer records `slot` as the winner of a game.
pub async fn win(bridge: &Bridge, provider: &str, organizer: &str, id: &str, slot: u8, key: &str) {
    let (_, found) = bridge.get(provider, &format!("/v1/matches/{id}")).await;
    let (status, decided) = bridge
        .post_keyed(
            organizer,
            &format!("/v1/matches/{id}/adjudications"),
            json!({ "kind": "game_result", "winner_slot": slot, "reason": "test", "expected_revision": found["revision"] }),
            Some(key),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{decided}");
}

/// Voids the match's last accepted game, as an organizer correcting it.
pub async fn void_last(
    bridge: &Bridge,
    provider: &str,
    organizer: &str,
    id: &str,
    key: &str,
) -> (StatusCode, Json) {
    let (_, found) = bridge.get(provider, &format!("/v1/matches/{id}")).await;
    let last = found["attempts"]
        .as_array()
        .unwrap()
        .iter()
        .rev()
        .find(|attempt| attempt["state"] == "accepted")
        .unwrap()["attempt_id"]
        .clone();
    bridge
        .post_keyed(
            organizer,
            &format!("/v1/matches/{id}/adjudications"),
            json!({ "kind": "void_game", "attempt_id": last, "reason": "Wrong game", "expected_revision": found["revision"] }),
            Some(key),
        )
        .await
}

/// A match's `delivery_state` and attempts.
pub async fn delivery(bridge: &Bridge, id: &str) -> (String, u32) {
    let id = id.to_owned();
    bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT delivery_state, delivery_attempts FROM matches WHERE id = ?1",
                [&id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?)
        })
        .await
        .unwrap()
}

/// Runs delivery passes until the match's delivery is `done`, for at most
/// five seconds. The bridge's own worker runs alongside; either may send.
pub async fn until(bridge: &Bridge, id: &str, done: impl Fn((String, u32)) -> bool) {
    for _ in 0..250 {
        if done(delivery(bridge, id).await) {
            return;
        }
        ember_bridge::deliver_results(bridge.state()).await;
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    panic!("delivery did not settle");
}
