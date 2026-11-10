//! What the connector takes as evidence before it ends a match, sets a
//! result aside or reports one: only what a set shows when asked for on its
//! own, never its absence from a list; the players a match was made for;
//! and the bridge's own record when a result never arrives. And a creation
//! interrupted by a crash is repeated, never doubled.
mod support;

use axum::http::{Method, StatusCode};
use serde_json::{Value, json};
use support::{
    BRACKET, Setup, World, creation,
    service::{discord, entry},
};

const LIST: &str = "GET /sets?state=open";
const READ: &str = "GET /sets/s-1";
const REPORT: &str = "POST /sets/s-1/report";
const ENTRIES: [&str; 2] = ["player1", "player2"];

async fn connected(setup: Setup) -> World {
    let world = World::start(setup).await;
    world.bridge.connect(1).await;
    world.bridge.connect(2).await;
    world
}

/// A world where s-1 has its first match, and the request log is empty.
async fn playing() -> (World, String) {
    let world = connected(Setup::default()).await;
    world.connector.pass(true).await;
    world.service.log();
    let match_id = world.bridge.matches().await[0].1.clone();
    (world, match_id)
}

fn winner(world: &World) -> Value {
    world
        .service
        .with(|state| state.set("s-1")["winner"].clone())
}

#[tokio::test]
async fn a_set_missing_from_a_read_is_asked_for_before_anything_is_ended() {
    let (world, match_id) = playing().await;
    world
        .service
        .with(|state| state.unlisted.insert("s-1".into()));

    // Left out of the list, still open on its own: the match goes on.
    world.connector.pass(true).await;
    assert_eq!(world.service.log(), [LIST, READ]);
    assert_eq!(world.snapshot(&match_id).await["state"], "awaiting_players");

    // Its result is reported once the set itself says it is still open.
    world.play(&match_id, &[0, 1, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(world.service.log(), [LIST, READ, REPORT]);
    assert_eq!(winner(&world), "player1");
}

#[tokio::test]
async fn a_report_whose_answer_was_lost_is_not_sent_again() {
    let (world, match_id) = playing().await;
    world.service.with(|state| state.lose_report_answer = true);
    world.play(&match_id, &[1, 1]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(world.service.log(), [LIST, REPORT]);

    // A minute later the set shows that very result: it is recorded as
    // reported, and nothing is sent.
    world.advance(60);
    world.connector.pass(false).await;
    assert_eq!(world.service.log(), [LIST, READ]);
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
    world.advance(60);
    world.connector.pass(false).await;
    assert!(world.service.log().is_empty());
    assert_eq!(world.service.reports().len(), 1);
}

#[tokio::test]
async fn a_result_the_service_refuses_is_left_to_the_organizer() {
    let (world, match_id) = playing().await;
    world.service.with(|state| {
        state
            .fail_reports
            .push_back(StatusCode::UNPROCESSABLE_ENTITY)
    });
    world.play(&match_id, &[0, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(world.service.log(), [LIST, REPORT]);
    assert_eq!(world.progress(BRACKET, "s-1"), "stuck");

    // Not sent again, and the set, still open, is not played again.
    world.advance(3_600);
    world.connector.pass(true).await;
    assert_eq!(world.service.log(), [LIST]);
    assert_eq!(world.bridge.matches().await.len(), 1);
}

#[tokio::test]
async fn an_expired_match_is_played_again() {
    let (world, match_id) = playing().await;
    world.advance(24 * 60 * 60 + 60);
    ember_bridge::maintain(world.bridge.state()).await;
    assert_eq!(world.snapshot(&match_id).await["state"], "expired");
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    assert_eq!(matches[1].0, "http:weekly:s-1:2");
}

/// The organizer puts Player 3 in Player 2's place in s-1.
fn swap_in_player3(world: &World) {
    world
        .service
        .with(|state| state.set("s-1")["entries"][1] = entry(3));
}

#[tokio::test]
async fn a_changed_entry_ends_the_match_and_its_late_result_is_ignored() {
    let (world, first) = playing().await;
    let player3 = world.bridge.connect(3).await;

    swap_in_player3(&world);
    world.connector.pass(true).await;
    assert_eq!(world.snapshot(&first).await["state"], "cancelled");
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    assert_eq!(matches[1].0, "http:weekly:s-1:2");
    let second = world.snapshot(&matches[1].1).await;
    assert_eq!(second["participants"][1]["ember_id"], player3.as_str());

    // The first match's restart arrives after the second attempt exists.
    world.service.log();
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(world.service.log(), [LIST]);
    assert_eq!(world.bridge.matches().await.len(), 2);
}

#[tokio::test]
async fn a_result_the_bridge_cannot_deliver_is_read_from_the_bridge() {
    let world = connected(Setup {
        serve_results: false,
        ..Setup::default()
    })
    .await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[1, 0, 1]).await;
    world.bridge.fail_delivery(&match_id).await;
    world.service.log();

    // A scheduled read finds nothing to do. Five minutes on, the connector
    // asks the bridge, finds the result being sent and not getting through,
    // and reports.
    world.advance(299);
    world.connector.pass(false).await;
    assert_eq!(world.service.log(), [LIST]);
    world.advance(1);
    world.connector.pass(false).await;
    assert_eq!(world.service.log(), [LIST, REPORT]);
    assert_eq!(winner(&world), "player2");
}

#[tokio::test]
async fn a_creation_the_bridge_never_received_is_sent_after_a_restart() {
    let world = connected(Setup::default()).await;
    let (status, found) = world
        .bridge
        .send(
            &world.provider,
            Method::POST,
            "/v1/players/lookup",
            Some(json!({ "discord": [discord(1), discord(2)] })),
            None,
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{found}");
    let players: Vec<Value> = [1, 2]
        .into_iter()
        .enumerate()
        .map(|(slot, n)| {
            let player = found["players"]
                .as_array()
                .unwrap()
                .iter()
                .find(|player| player["discord_user_id"] == discord(n))
                .unwrap();
            json!({ "participant_id": player["participant_id"], "ember_id": player["ember_id"], "slot": slot })
        })
        .collect();
    world.stage_waiting(
        BRACKET,
        "s-1",
        ENTRIES,
        &creation(
            "http:weekly:s-1:1",
            &players,
            2,
            json!({ "round_label": "Round 1" }),
        ),
    );

    world.restarted().pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    assert_eq!(matches[0].0, "http:weekly:s-1:1");
}

#[tokio::test]
async fn a_match_made_before_a_crash_is_found_not_doubled_and_ended_if_its_set_changed() {
    let (world, first) = playing().await;
    world
        .stage_unanswered(BRACKET, "s-1", ENTRIES, &first)
        .await;

    // Unchanged: sending it again finds the same match.
    let restarted = world.restarted();
    restarted.pass(true).await;
    assert_eq!(world.bridge.matches().await.len(), 1);

    // Changed while the connector was down: the old match is found, then
    // ended, and the new pair plays a new attempt. The old match's end is
    // not taken for the new pair's.
    world
        .stage_unanswered(BRACKET, "s-1", ENTRIES, &first)
        .await;
    swap_in_player3(&world);
    world.bridge.connect(3).await;
    let restarted = world.restarted();
    restarted.pass(true).await;
    restarted.pass(true).await;
    assert_eq!(world.snapshot(&first).await["state"], "cancelled");
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    assert_eq!(matches[1].0, "http:weekly:s-1:2");
    world.service.log();
    world.bridge.deliver().await;
    restarted.pass(false).await;
    let log = world.service.log();
    assert!(!log.contains(&REPORT.to_owned()), "{log:?}");
}

/// A world where s-1's match was made, and the bridge's answer to its
/// creation was lost: the attempt waits, with its creation saved.
async fn unanswered() -> (World, String) {
    let (world, first) = playing().await;
    world
        .stage_unanswered(BRACKET, "s-1", ENTRIES, &first)
        .await;
    (world, first)
}

#[tokio::test]
async fn an_unanswered_creation_whose_set_was_decided_has_its_match_cancelled() {
    let (world, first) = unanswered().await;
    // The organizer decides s-1 while the connector is down: it is no
    // longer listed open, and the match the bridge made for it would wait
    // for players until it expired.
    world.service.with(|state| state.decide("s-1", 0));
    world.restarted().pass(true).await;
    assert_eq!(world.snapshot(&first).await["state"], "cancelled");
    assert_eq!(world.progress(BRACKET, "s-1"), "closed");
    assert_eq!(world.bridge.matches().await.len(), 1);
}

#[tokio::test]
async fn an_unanswered_creation_whose_set_was_removed_has_its_match_cancelled() {
    let (world, first) = unanswered().await;
    world
        .service
        .with(|state| state.sets.retain(|set| set["id"] != "s-1"));
    world.restarted().pass(true).await;
    assert_eq!(world.snapshot(&first).await["state"], "cancelled");
    // Played again, as a new attempt, if it ever opens again.
    assert_eq!(world.progress(BRACKET, "s-1"), "waiting");
}
