//! What the connector takes as evidence before it ends a match, sets a
//! result aside or reports one: only what a set shows when asked for on its
//! own, never its absence from a paged list; the players a match was made
//! for; and the bridge's own record when a result never arrives. And a
//! creation interrupted by a crash is repeated, never doubled.
mod support;

use axum::http::{Method, StatusCode};
use serde_json::{Value, json};
use support::{Setup, World, challonge::lines};

const LIST: &str = "GET /matches.json?state=open&page=1&per_page=25";
const READ_1001: &str = "GET /matches/1001.json";
const REPORT_1001: &str = "PUT /matches/1001.json";

async fn connected(setup: Setup) -> World {
    let world = World::start(setup).await;
    world.bridge.connect(1, "Kate").await;
    world.bridge.connect(2, "sam_99").await;
    world
}

/// A world where 1001 has its first match, and the request log is empty.
async fn playing() -> (World, String) {
    let world = connected(Setup::default()).await;
    world.connector.pass(true).await;
    world.challonge.log();
    let match_id = world.bridge.matches().await[0].1.clone();
    (world, match_id)
}

#[tokio::test]
async fn a_set_missing_from_a_read_is_asked_for_before_anything_is_ended() {
    let (world, match_id) = playing().await;
    world
        .challonge
        .state
        .lock()
        .unwrap()
        .hidden
        .insert("1001".into());

    // Left out of the list, still open on its own: the match goes on.
    world.connector.pass(true).await;
    assert_eq!(lines(&world.challonge.log()), [LIST, READ_1001]);
    assert_eq!(world.snapshot(&match_id).await["state"], "awaiting_players");

    // Its result is reported once the set itself says it is still open.
    world.play(&match_id, &[0, 1, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, READ_1001, REPORT_1001]
    );
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("355"));
}

#[tokio::test]
async fn a_report_whose_answer_was_lost_is_not_sent_again() {
    let (world, match_id) = playing().await;
    world.challonge.state.lock().unwrap().lose_report_answers = true;
    world.play(&match_id, &[1, 1]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(lines(&world.challonge.log()), [LIST, REPORT_1001]);

    // A minute later the set shows that very score: it is recorded as
    // reported, and nothing is sent.
    world.challonge.state.lock().unwrap().lose_report_answers = false;
    world.advance(60);
    world.connector.pass(false).await;
    assert_eq!(lines(&world.challonge.log()), [LIST, READ_1001]);
    world.advance(60);
    world.connector.pass(false).await;
    assert!(world.challonge.log().is_empty());
}

#[tokio::test]
async fn a_score_the_site_refuses_is_left_to_the_organizer() {
    let (world, match_id) = playing().await;
    world.challonge.state.lock().unwrap().refuse_reports = true;
    world.play(&match_id, &[0, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(lines(&world.challonge.log()), [LIST, REPORT_1001]);

    // Not sent again, and the set, still open, is not played again.
    world.advance(3_600);
    world.connector.pass(true).await;
    assert_eq!(lines(&world.challonge.log()), [LIST]);
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
    assert_eq!(matches[1].0, "challonge:ember_weekly:1001:2");
}

#[tokio::test]
async fn a_changed_entry_ends_the_match_and_its_late_result_is_ignored() {
    let (world, first) = playing().await;
    let ryu = world.bridge.connect(3, "ryu").await;

    // The organizer puts Ryu in Sam's place.
    world.challonge.state.lock().unwrap().matches[0].players[1] = Some("353".into());
    world.connector.pass(true).await;
    assert_eq!(world.snapshot(&first).await["state"], "cancelled");
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    assert_eq!(matches[1].0, "challonge:ember_weekly:1001:2");
    let second = world.snapshot(&matches[1].1).await;
    assert_eq!(second["participants"][1]["ember_id"], ryu.as_str());

    // The first match's restart arrives after the second attempt exists.
    world.challonge.log();
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(lines(&world.challonge.log()), [LIST]);
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
    world.challonge.log();

    // A scheduled read finds nothing to do. Five minutes on, the connector
    // asks the bridge, finds the result being sent and not getting through,
    // and reports.
    world.advance(299);
    world.connector.pass(false).await;
    assert_eq!(lines(&world.challonge.log()), [LIST]);
    world.advance(1);
    world.connector.pass(false).await;
    assert_eq!(lines(&world.challonge.log()), [LIST, REPORT_1001]);
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("354"));
}

/// The creation the connector sends for an attempt, rebuilt from what the
/// bridge has for its participants.
fn creation(external: &str, players: &[Value]) -> Value {
    json!({
        "external_match_id": external,
        "game": "usf4",
        "participants": players,
        "rules": {
            "games_to_win": 2,
            "draw_policy": "replay_no_score",
            "native_rules_profile": "ember-room-v1",
            "edition_policy": "ultra_only",
            "character_policy": "unrestricted_between_games",
            "stage_policy": "p1_selects",
            "input_delay_policy": "ember_existing_ready_policy",
        },
        "observer_policy": "authorized_only",
        "result_policy": "two_player_agreement_or_review",
        "required_build_id": "test-build",
        "metadata": { "round_label": "Round 1" },
    })
}

/// What a crash leaves when it comes after a creation was saved and before
/// the bridge's answer was: the attempt waiting, with its creation.
fn stage_waiting(world: &World, entries: [&str; 2], command: &Value) {
    let players = &command["participants"];
    world
        .database()
        .execute(
            "INSERT INTO sets (bracket, set_id, player1, player2, ember1, ember2, attempt,
                 match_id, progress, command)
             VALUES ('challonge:ember_weekly', '1001', ?1, ?2, ?3, ?4, 1, NULL, 'waiting', ?5)
             ON CONFLICT (bracket, set_id) DO UPDATE SET player1 = ?1, player2 = ?2,
                 ember1 = ?3, ember2 = ?4, attempt = 1, match_id = NULL,
                 progress = 'waiting', command = ?5",
            rusqlite::params![
                entries[0],
                entries[1],
                players[0]["ember_id"].as_str().unwrap(),
                players[1]["ember_id"].as_str().unwrap(),
                command.to_string()
            ],
        )
        .unwrap();
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
            Some(json!({ "accounts": [
                { "site": "challonge", "id": "Kate" },
                { "site": "challonge", "id": "sam_99" },
            ] })),
            None,
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{found}");
    let players: Vec<Value> = found["accounts"]
        .as_array()
        .unwrap()
        .iter()
        .enumerate()
        .map(|(slot, player)| {
            json!({ "participant_id": player["participant_id"], "ember_id": player["ember_id"], "slot": slot })
        })
        .collect();
    stage_waiting(
        &world,
        ["355", "354"],
        &creation("challonge:ember_weekly:1001:1", &players),
    );

    world.restarted().pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    assert_eq!(matches[0].0, "challonge:ember_weekly:1001:1");
}

#[tokio::test]
async fn a_match_made_before_a_crash_is_found_not_doubled_and_ended_if_its_set_changed() {
    let (world, first) = playing().await;
    let found = world.snapshot(&first).await;
    let players: Vec<Value> = found["participants"]
        .as_array()
        .unwrap()
        .iter()
        .map(|player| {
            json!({ "participant_id": player["participant_id"], "ember_id": player["ember_id"], "slot": player["slot"] })
        })
        .collect();
    let command = creation("challonge:ember_weekly:1001:1", &players);
    stage_waiting(&world, ["355", "354"], &command);

    // Unchanged: sending it again finds the same match.
    let restarted = world.restarted();
    restarted.pass(true).await;
    assert_eq!(world.bridge.matches().await.len(), 1);

    // Changed while the connector was down: the old match is found, then
    // ended, and the new pair plays a new attempt. The old match's end is
    // not taken for the new pair's.
    stage_waiting(&world, ["355", "354"], &command);
    world.challonge.state.lock().unwrap().matches[0].players[1] = Some("353".into());
    world.bridge.connect(3, "ryu").await;
    let restarted = world.restarted();
    restarted.pass(true).await;
    restarted.pass(true).await;
    assert_eq!(world.snapshot(&first).await["state"], "cancelled");
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    assert_eq!(matches[1].0, "challonge:ember_weekly:1001:2");
    world.challonge.log();
    world.bridge.deliver().await;
    restarted.pass(false).await;
    let log = lines(&world.challonge.log());
    assert!(!log.iter().any(|line| line.starts_with("PUT")), "{log:?}");
}
