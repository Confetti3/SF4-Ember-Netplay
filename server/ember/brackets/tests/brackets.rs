//! The connector between a fake Challonge and a real bridge: sets become
//! Ember matches, scores go back, restarts make fresh attempts, and sets
//! the organizer changes are left to the organizer.
mod support;

use ember_brackets::{CredentialKind, RoundRule, Rounds, Side};
use serde_json::json;
use support::{
    Setup, World,
    challonge::{FakeMatch, lines},
};

const LIST: &str = "GET /matches.json?state=open&page=1&per_page=25";
const PARTICIPANTS: &str = "GET /participants.json?page=1&per_page=25";

#[tokio::test]
async fn a_set_is_played_in_ember_and_its_score_goes_back() {
    let world = World::start(Setup::default()).await;
    // Usernames are matched without case.
    let player1 = world.bridge.connect(1, "player1").await;
    let player2 = world.bridge.connect(2, "Player_2").await;

    world.connector.pass(true).await;
    let log = world.challonge.log();
    assert_eq!(
        lines(&log),
        [LIST, PARTICIPANTS, "PUT /matches/1001/change_state.json"]
    );
    assert_eq!(
        log[2].body,
        json!({ "data": { "type": "MatchState", "attributes": { "state": "mark_as_underway" } } })
    );
    assert!(world.challonge.find("1001").underway);
    // Only 1001 has two players with Ember.
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    let (external, match_id, _) = &matches[0];
    assert_eq!(external, "challonge:ember_weekly:1001:1");
    let found = world.snapshot(match_id).await;
    assert_eq!(found["games_to_win"], 2);
    assert_eq!(found["rules"]["native_rules_profile"], "ember-room-v1");
    assert_eq!(found["metadata"], json!({ "round_label": "Round 1" }));
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
    assert_eq!(lines(&world.challonge.log()), [LIST]);
    assert_eq!(world.bridge.matches().await.len(), 1);

    // Player 2 wins 2-1. The result is read against the bracket at once, though
    // no read is due, and reported.
    world.play(match_id, &[1, 0, 1]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    let log = world.challonge.log();
    assert_eq!(lines(&log), [LIST, "PUT /matches/1001.json"]);
    assert_eq!(
        log[1].body,
        json!({ "data": { "type": "match", "attributes": { "match": [
            { "participant_id": "355", "score_set": "1", "rank": 2, "advancing": false },
            { "participant_id": "354", "score_set": "2", "rank": 1, "advancing": true },
        ] } } })
    );
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("354"));
    // A result is settled once.
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(lines(&world.challonge.log()), Vec::<String>::new());

    // The organizer decides 1002 by hand: 1003 opens, Player 2 against Player 3, who
    // has not connected Challonge in Ember yet.
    world.challonge.decide("1002", 0);
    world.connector.pass(true).await;
    assert_eq!(lines(&world.challonge.log()), [LIST]);
    assert_eq!(world.bridge.matches().await.len(), 1);
    let player3 = world.bridge.connect(3, "player3").await;
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    let (external, match_id, _) = &matches[1];
    assert_eq!(external, "challonge:ember_weekly:1003:1");
    let found = world.snapshot(match_id).await;
    // Round 2 is first to 3.
    assert_eq!(found["games_to_win"], 3);
    assert_eq!(found["metadata"]["round_label"], "Round 2");
    assert_eq!(found["participants"][0]["ember_id"], player2.as_str());
    assert_eq!(found["participants"][1]["ember_id"], player3.as_str());
}

#[tokio::test]
async fn a_restarted_connector_carries_on_without_making_a_match_twice() {
    let world = World::start(Setup::default()).await;
    world.bridge.connect(1, "Player1").await;
    world.bridge.connect(2, "player_2").await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();

    let restarted = world.restarted();
    restarted.pass(true).await;
    assert_eq!(world.bridge.matches().await.len(), 1);
    world.challonge.log();

    // The first one receives the result; the restarted one reports it.
    world.play(&match_id, &[0, 0]).await;
    world.bridge.deliver().await;
    restarted.pass(false).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, "PUT /matches/1001.json"]
    );
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("355"));
}

#[tokio::test]
async fn a_restart_makes_a_fresh_attempt_and_a_set_changed_by_hand_is_cancelled() {
    let world = World::start(Setup::default()).await;
    world.bridge.connect(1, "Player1").await;
    world.bridge.connect(2, "player_2").await;
    world.connector.pass(true).await;
    let first = world.bridge.matches().await[0].1.clone();
    world.challonge.log();

    // A disputed game cancels the match: the set is played again, from the
    // same read, and is not marked underway again.
    assert!(world.challonge.find("1001").underway);
    world.cancel(&first).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(lines(&world.challonge.log()), [LIST]);
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    let (external, second, _) = &matches[1];
    assert_eq!(external, "challonge:ember_weekly:1001:2");

    // The organizer reports the set by hand: the live match is cancelled,
    // and its restart is not played again or reported.
    world.challonge.decide("1001", 0);
    world.connector.pass(true).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, "GET /matches/1001.json"]
    );
    assert_eq!(world.snapshot(second).await["state"], "cancelled");
    world.bridge.deliver().await;
    world.connector.pass(true).await;
    let log = lines(&world.challonge.log());
    assert!(!log.iter().any(|line| line.starts_with("PUT")), "{log:?}");
    assert_eq!(world.bridge.matches().await.len(), 2);
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("355"));
}

#[tokio::test]
async fn a_set_that_keeps_restarting_is_left_to_the_organizer() {
    let world = World::start(Setup::default()).await;
    world.bridge.connect(1, "Player1").await;
    world.bridge.connect(2, "player_2").await;
    world.connector.pass(true).await;
    for attempt in 1..=5 {
        let matches = world.bridge.matches().await;
        assert_eq!(matches.len(), attempt);
        let (external, match_id, _) = &matches[attempt - 1];
        assert_eq!(external, &format!("challonge:ember_weekly:1001:{attempt}"));
        world.cancel(match_id).await;
        world.bridge.deliver().await;
        world.connector.pass(false).await;
    }
    world.connector.pass(true).await;
    assert_eq!(world.bridge.matches().await.len(), 5);
    assert_eq!(world.challonge.find("1001").state, "open");
}

#[tokio::test]
async fn a_set_the_organizer_already_reported_is_not_reported_again() {
    let world = World::start(Setup::default()).await;
    world.bridge.connect(1, "Player1").await;
    world.bridge.connect(2, "player_2").await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[0, 0]).await;
    world.challonge.decide("1001", 1);
    world.challonge.log();

    // The set is gone from the read, so it is asked for on its own, and its
    // winner is not the match's.
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, "GET /matches/1001.json"]
    );
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("354"));
}

#[tokio::test]
async fn long_lists_are_read_page_by_page_with_an_oauth_token() {
    let participants: Vec<(String, Option<String>)> = (0..60)
        .map(|n| ((5000 + n).to_string(), Some(format!("player{n}"))))
        .collect();
    let matches = (0..30)
        .map(|n| {
            FakeMatch::open(
                &(7000 + n).to_string(),
                1,
                [&participants[2 * n].0, &participants[2 * n + 1].0],
            )
        })
        .collect();
    let world = World::start(Setup {
        kind: CredentialKind::Bearer,
        participants,
        matches,
        ..Setup::default()
    })
    .await;
    for n in 0..60u8 {
        world.bridge.connect(10 + n, &format!("Player{n}")).await;
    }

    world.connector.pass(true).await;
    let log = world.challonge.log();
    let reads: Vec<String> = lines(&log)
        .into_iter()
        .filter(|line| line.starts_with("GET"))
        .collect();
    assert_eq!(
        reads,
        [
            LIST,
            "GET /matches.json?state=open&page=2&per_page=25",
            PARTICIPANTS,
            "GET /participants.json?page=2&per_page=25",
            "GET /participants.json?page=3&per_page=25",
        ]
    );
    let headers = &log[0].headers;
    assert_eq!(headers["authorization-type"], "v2");
    assert_eq!(headers["authorization"], "Bearer test-challonge-api-key");
    // Sixty usernames take two lookups (the bridge refuses more than 32 in
    // one), and every set gets its match.
    assert_eq!(world.bridge.matches().await.len(), 30);
}

#[tokio::test]
async fn a_set_reopened_for_the_same_players_is_left_to_the_organizer() {
    let world = World::start(Setup::default()).await;
    world.bridge.connect(1, "player1").await;
    world.bridge.connect(2, "player_2").await;
    world.bridge.connect(3, "player3").await;
    // 1001 is played in Ember (Player 2 wins), 1002 decided by hand, and
    // 1003, Player 2 against Player 3, played in Ember too.
    world.connector.pass(true).await;
    let first = world.bridge.matches().await[0].1.clone();
    world.play(&first, &[1, 1]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    world.challonge.decide("1002", 0);
    world.advance(601);
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    world.play(&matches[1].1, &[1, 1, 1]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(world.progress("challonge:ember_weekly", "1003"), "reported");

    // The organizer reopens 1001 to correct it, which reopens 1003 after
    // it once 1001 is decided again, for the same two players: both are the
    // organizer's, and no Ember match is made for either.
    world.challonge.reopen("1001");
    world.advance(601);
    world.connector.pass(true).await;
    world.challonge.decide("1001", 1);
    assert_eq!(world.challonge.find("1003").state, "open");
    world.advance(601);
    world.connector.pass(true).await;
    assert_eq!(world.bridge.matches().await.len(), 2);
    assert_eq!(world.progress("challonge:ember_weekly", "1001"), "reported");
    assert_eq!(world.progress("challonge:ember_weekly", "1003"), "reported");

    // Corrected the other way, 1003 has other players: a new attempt.
    world.challonge.reopen("1001");
    world.challonge.decide("1001", 0);
    world.advance(601);
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 3);
    assert_eq!(matches[2].0, "challonge:ember_weekly:1003:2");
}

#[tokio::test]
async fn a_set_listed_twice_is_started_once() {
    let world = World::start(Setup::default()).await;
    world.bridge.connect(1, "player1").await;
    world.bridge.connect(2, "player_2").await;
    world
        .challonge
        .state
        .lock()
        .unwrap()
        .listed_twice
        .insert("1001".into());
    world.connector.pass(true).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, PARTICIPANTS, "PUT /matches/1001/change_state.json"]
    );
    assert_eq!(world.bridge.matches().await.len(), 1);
}

#[tokio::test]
async fn a_score_challonge_refuses_for_its_format_is_sent_again() {
    let world = World::start(Setup::default()).await;
    world.bridge.connect(1, "player1").await;
    world.bridge.connect(2, "player_2").await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[0, 0]).await;
    world.bridge.deliver().await;
    world.challonge.log();
    // 415 says the request's format was wrong, not that this set cannot
    // take a score: the score waits, and is not given up.
    world.challonge.state.lock().unwrap().report_failures = vec![415, 406];
    world.connector.pass(false).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, "PUT /matches/1001.json"]
    );
    assert_eq!(world.progress("challonge:ember_weekly", "1001"), "live");
    world.advance(61);
    world.connector.pass(false).await;
    world.advance(121);
    world.connector.pass(false).await;
    assert_eq!(world.progress("challonge:ember_weekly", "1001"), "reported");
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("355"));
}

#[tokio::test]
async fn a_live_set_reset_by_a_reopen_is_played_again_when_it_opens() {
    let world = World::start(Setup::default()).await;
    world.bridge.connect(1, "player1").await;
    world.bridge.connect(2, "player_2").await;
    world.bridge.connect(3, "player3").await;
    // 1001 is played in Ember (Player 2 wins), 1002 decided by hand, and
    // 1003, Player 2 against Player 3, is live.
    world.connector.pass(true).await;
    let first = world.bridge.matches().await[0].1.clone();
    world.play(&first, &[1, 1]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    world.challonge.decide("1002", 0);
    world.advance(601);
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    let live = matches[1].1.clone();

    // Reopening 1001 sends 1003 back to pending: its match is cancelled
    // before anyone played it.
    world.challonge.reopen("1001");
    world.advance(601);
    world.connector.pass(true).await;
    assert_eq!(world.snapshot(&live).await["state"], "cancelled");
    assert_eq!(world.progress("challonge:ember_weekly", "1003"), "waiting");

    // 1001 is entered again with the same winner: 1003 opens for the same
    // two players, who have not played it, so it gets a new attempt.
    world.challonge.decide("1001", 1);
    world.advance(601);
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 3);
    assert_eq!(matches[2].0, "challonge:ember_weekly:1003:2");
}

#[tokio::test]
async fn a_saved_creation_that_cannot_be_read_ends_only_its_set() {
    let mut setup = Setup::default();
    setup.participants[3].1 = Some("player4".into());
    let world = World::start(setup).await;
    for (byte, name) in [
        (1, "player1"),
        (2, "player_2"),
        (3, "player3"),
        (4, "player4"),
    ] {
        world.bridge.connect(byte, name).await;
    }
    // What an upgrade that changed the creation's form leaves: 1001's
    // creation was saved, never answered, and no longer reads.
    world
        .database()
        .execute(
            "INSERT INTO sets (bracket, set_id, player1, player2, attempt, progress, command)
             VALUES ('challonge:ember_weekly', '1001', '355', '354', 1, 'waiting',
                     '{\"not\": \"a creation\"}')",
            [],
        )
        .unwrap();
    world.connector.pass(true).await;
    // 1001 goes to the organizer; 1002, after it, is played all the same.
    assert_eq!(world.progress("challonge:ember_weekly", "1001"), "stuck");
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    assert_eq!(matches[0].0, "challonge:ember_weekly:1002:1");
}

#[tokio::test]
async fn a_score_challonge_does_not_show_recorded_is_checked() {
    let world = World::start(Setup::default()).await;
    world.bridge.connect(1, "player1").await;
    world.bridge.connect(2, "player_2").await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[0, 0]).await;
    world.bridge.deliver().await;
    world.challonge.log();
    // Challonge answers 200 without the match, and the score did not go
    // in: read on its own, the match is still open, so the score waits.
    world.challonge.state.lock().unwrap().odd_report_answers = vec![(json!({}), false)];
    world.connector.pass(false).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, "PUT /matches/1001.json", "GET /matches/1001.json"]
    );
    assert_eq!(world.progress("challonge:ember_weekly", "1001"), "live");
    assert_eq!(world.challonge.find("1001").winner, None);

    // Again without the match, but the score went in: the read shows it.
    world.challonge.state.lock().unwrap().odd_report_answers =
        vec![(json!({ "data": null }), true)];
    world.advance(61);
    world.connector.pass(false).await;
    assert_eq!(world.progress("challonge:ember_weekly", "1001"), "reported");
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("355"));
}

#[tokio::test]
async fn challonge_group_and_final_stages_take_their_own_length() {
    // A two stage tournament: 1001 is in a group, 1002 in the final stage,
    // both round 1. The final stage is first to 3.
    let mut group = FakeMatch::open("1001", 1, ["355", "354"]);
    group.group_id = Some(7_001);
    let mut setup = Setup {
        matches: vec![group, FakeMatch::open("1002", 1, ["353", "352"])],
        rounds: Rounds {
            games_to_win: 2,
            rules: vec![RoundRule {
                phase: Some("final".into()),
                from_round: 1,
                to_round: None,
                side: Side::Any,
                games_to_win: 3,
            }],
        },
        ..Setup::default()
    };
    setup.participants[3].1 = Some("player4".into());
    let world = World::start(setup).await;
    for (byte, name) in [
        (1, "player1"),
        (2, "player_2"),
        (3, "player3"),
        (4, "player4"),
    ] {
        world.bridge.connect(byte, name).await;
    }
    world.connector.pass(true).await;
    let mut found = Vec::new();
    for (external, match_id, _) in world.bridge.matches().await {
        let snapshot = world.snapshot(&match_id).await;
        found.push((
            external,
            snapshot["games_to_win"].clone(),
            snapshot["metadata"]["round_label"].clone(),
        ));
    }
    found.sort_by(|a, b| a.0.cmp(&b.0));
    assert_eq!(
        found,
        [
            (
                "challonge:ember_weekly:1001:1".to_owned(),
                json!(2),
                json!("Group stage, Round 1")
            ),
            (
                "challonge:ember_weekly:1002:1".to_owned(),
                json!(3),
                json!("Round 1")
            ),
        ]
    );
}
