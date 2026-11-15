//! The connector between a fake bracket service and a real bridge: sets
//! become Ember matches, results go back, restarts make fresh attempts, and
//! sets the organizer changes are left to the organizer.
mod support;

use serde_json::json;
use support::{BRACKET, Setup, World, service::open};

const LIST: &str = "GET /sets?state=open";
const UNDERWAY: &str = "POST /sets/s-1/underway";
const READ: &str = "GET /sets/s-1";
const REPORT: &str = "POST /sets/s-1/report";

/// A world where players 1 and 2 are connected in Ember.
async fn connected() -> World {
    let world = World::start(Setup::default()).await;
    world.bridge.connect(1).await;
    world.bridge.connect(2).await;
    world
}

/// The winning entry of set `id`, as the service has it.
fn winner(world: &World, id: &str) -> serde_json::Value {
    world.service.with(|state| state.set(id)["winner"].clone())
}

#[tokio::test]
async fn a_set_is_played_in_ember_and_its_score_goes_back() {
    let world = World::start(Setup::default()).await;
    let player1 = world.bridge.connect(1).await;
    let player2 = world.bridge.connect(2).await;

    world.connector.pass(true).await;
    assert_eq!(world.service.log(), [LIST, UNDERWAY]);
    // Only s-1 has two players with Ember.
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    let (external, match_id, _) = &matches[0];
    assert_eq!(external, "http:weekly:s-1:1");
    let found = world.snapshot(match_id).await;
    assert_eq!(found["games_to_win"], 2);
    assert_eq!(found["rules"]["native_rules_profile"], "ember-room-v1");
    assert_eq!(found["metadata"]["round_label"], "Round 1");
    let roster: Vec<(String, u64)> = found["participants"]
        .as_array()
        .unwrap()
        .iter()
        .map(|p| {
            (
                p["ember_id"].as_str().unwrap().to_owned(),
                p["slot"].as_u64().unwrap(),
            )
        })
        .collect();
    assert_eq!(
        roster,
        [
            (player1.as_str().to_owned(), 0),
            (player2.as_str().to_owned(), 1)
        ]
    );

    // Reading again makes nothing new.
    world.connector.pass(true).await;
    assert_eq!(world.service.log(), [LIST]);
    assert_eq!(world.bridge.matches().await.len(), 1);

    // Player 2 wins 2-1. The result is read against the bracket at once,
    // though no read is due, and reported.
    world.play(match_id, &[1, 0, 1]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(world.service.log(), [LIST, REPORT]);
    assert_eq!(winner(&world, "s-1"), "player2");
    // A result is settled once.
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert!(world.service.log().is_empty());

    // The organizer decides s-2 by hand: s-3 opens, Player 2 against
    // Player 3, who has not connected in Ember yet.
    world.service.with(|state| state.decide("s-2", 0));
    world.connector.pass(true).await;
    assert_eq!(world.service.log(), [LIST]);
    assert_eq!(world.bridge.matches().await.len(), 1);
    let player3 = world.bridge.connect(3).await;
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    let (external, match_id, _) = &matches[1];
    assert_eq!(external, "http:weekly:s-3:1");
    let found = world.snapshot(match_id).await;
    // Round 2 is first to 3.
    assert_eq!(found["games_to_win"], 3);
    assert_eq!(found["metadata"]["round_label"], "Round 2");
    assert_eq!(found["participants"][0]["ember_id"], player2.as_str());
    assert_eq!(found["participants"][1]["ember_id"], player3.as_str());
}

#[tokio::test]
async fn a_restarted_connector_carries_on_without_making_a_match_twice() {
    let world = connected().await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();

    let restarted = world.restarted();
    restarted.pass(true).await;
    assert_eq!(world.bridge.matches().await.len(), 1);
    world.service.log();

    // The first one receives the result; the restarted one reports it.
    world.play(&match_id, &[0, 0]).await;
    world.bridge.deliver().await;
    restarted.pass(false).await;
    assert_eq!(world.service.log(), [LIST, REPORT]);
    assert_eq!(winner(&world, "s-1"), "player1");
}

#[tokio::test]
async fn a_restart_makes_a_fresh_attempt_and_a_set_changed_by_hand_is_cancelled() {
    let world = connected().await;
    world.connector.pass(true).await;
    let first = world.bridge.matches().await[0].1.clone();
    world.service.log();

    // A disputed game cancels the match: the set is played again, from the
    // same read, and marked underway again with the new match.
    world.cancel(&first).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(world.service.log(), [LIST, UNDERWAY]);
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    let (external, second, _) = &matches[1];
    assert_eq!(external, "http:weekly:s-1:2");

    // The organizer reports the set by hand: the live match is cancelled,
    // and its restart is not played again or reported.
    world.service.with(|state| state.decide("s-1", 0));
    world.connector.pass(true).await;
    assert_eq!(world.service.log(), [LIST, READ]);
    assert_eq!(world.snapshot(second).await["state"], "cancelled");
    world.bridge.deliver().await;
    world.connector.pass(true).await;
    let log = world.service.log();
    assert!(!log.iter().any(|line| line.starts_with("POST")), "{log:?}");
    assert_eq!(world.bridge.matches().await.len(), 2);
    assert!(world.service.reports().is_empty());
}

#[tokio::test]
async fn a_set_that_keeps_restarting_is_left_to_the_organizer() {
    let world = connected().await;
    world.connector.pass(true).await;
    for attempt in 1..=5 {
        let matches = world.bridge.matches().await;
        assert_eq!(matches.len(), attempt);
        let (external, match_id, _) = &matches[attempt - 1];
        assert_eq!(external, &format!("http:weekly:s-1:{attempt}"));
        world.cancel(match_id).await;
        world.bridge.deliver().await;
        world.connector.pass(false).await;
    }
    world.connector.pass(true).await;
    assert_eq!(world.bridge.matches().await.len(), 5);
    assert_eq!(world.progress(BRACKET, "s-1"), "stuck");
    assert!(world.service.reports().is_empty());
}

#[tokio::test]
async fn a_set_the_organizer_already_reported_is_not_reported_again() {
    let world = connected().await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[0, 0]).await;
    world.service.with(|state| state.decide("s-1", 1));
    world.service.log();

    // The set is gone from the read, so it is asked for on its own, and its
    // winner is not the match's.
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(world.service.log(), [LIST, READ]);
    assert_eq!(winner(&world, "s-1"), "player2");
    assert!(world.service.reports().is_empty());
}

#[tokio::test]
async fn sixty_players_are_looked_up_in_batches_and_every_set_is_made() {
    let sets = (0..30)
        .map(|n| open(&format!("s-{}", 100 + n), 1, [2 * n + 1, 2 * n + 2]))
        .collect();
    let world = World::start(Setup {
        sets,
        ..Setup::default()
    })
    .await;
    for n in 1..=60 {
        world.bridge.connect(n).await;
    }
    // Sixty accounts take two lookups (the bridge refuses more than 32 in
    // one), and every set gets its match.
    world.connector.pass(true).await;
    assert_eq!(world.bridge.matches().await.len(), 30);
}

/// Players 1 to 3 connected; s-1 played in Ember (Player 2 wins), s-2
/// decided by hand, and s-3, Player 2 against Player 3, made in Ember. Its
/// match ID.
async fn up_to_the_final() -> (World, String) {
    let world = connected().await;
    world.bridge.connect(3).await;
    world.connector.pass(true).await;
    let first = world.bridge.matches().await[0].1.clone();
    world.play(&first, &[1, 1]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    world.service.with(|state| state.decide("s-2", 0));
    world.advance(601);
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    assert_eq!(matches[1].0, "http:weekly:s-3:1");
    (world, matches[1].1.clone())
}

#[tokio::test]
async fn a_set_reopened_for_the_same_players_is_left_to_the_organizer() {
    let (world, last) = up_to_the_final().await;
    world.play(&last, &[1, 1, 1]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(world.progress(BRACKET, "s-3"), "reported");

    // The organizer reopens s-1 to correct it, which reopens s-3 after it
    // once s-1 is decided again, for the same two players: both are the
    // organizer's, and no Ember match is made for either.
    world.service.with(|state| state.reopen("s-1"));
    world.advance(601);
    world.connector.pass(true).await;
    world.service.with(|state| state.decide("s-1", 1));
    assert_eq!(
        world
            .service
            .with(|state| state.set("s-3")["status"].clone()),
        "open"
    );
    world.advance(601);
    world.connector.pass(true).await;
    assert_eq!(world.bridge.matches().await.len(), 2);
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
    assert_eq!(world.progress(BRACKET, "s-3"), "reported");

    // Corrected the other way, s-3 has other players: a new attempt.
    world.service.with(|state| {
        state.reopen("s-1");
        state.decide("s-1", 0);
    });
    world.advance(601);
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 3);
    assert_eq!(matches[2].0, "http:weekly:s-3:2");
}

#[tokio::test]
async fn a_live_set_reset_by_a_reopen_is_played_again_when_it_opens() {
    let (world, live) = up_to_the_final().await;

    // Reopening s-1 sends s-3 back to pending: its match is cancelled
    // before anyone played it.
    world.service.with(|state| state.reopen("s-1"));
    world.advance(601);
    world.connector.pass(true).await;
    assert_eq!(world.snapshot(&live).await["state"], "cancelled");
    assert_eq!(world.progress(BRACKET, "s-3"), "waiting");

    // s-1 is entered again with the same winner: s-3 opens for the same
    // two players, who have not played it, so it gets a new attempt.
    world.service.with(|state| state.decide("s-1", 1));
    world.advance(601);
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 3);
    assert_eq!(matches[2].0, "http:weekly:s-3:2");
}

#[tokio::test]
async fn a_saved_creation_that_cannot_be_read_ends_only_its_set() {
    let world = connected().await;
    world.bridge.connect(3).await;
    world.bridge.connect(4).await;
    // What an upgrade that changed the creation's form leaves: s-1's
    // creation was saved, never answered, and no longer reads.
    world
        .database()
        .execute(
            "INSERT INTO sets (bracket, set_id, player1, player2, attempt, progress, command)
             VALUES (?1, 's-1', 'player1', 'player2', 1, 'waiting', ?2)",
            [BRACKET, &json!({ "not": "a creation" }).to_string()],
        )
        .unwrap();
    world.connector.pass(true).await;
    // s-1 goes to the organizer; s-2, after it, is played all the same.
    assert_eq!(world.progress(BRACKET, "s-1"), "stuck");
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    assert_eq!(matches[0].0, "http:weekly:s-2:1");
}
