//! The `http` adapter between an organizer's own service (a fake answering
//! the contract in ADAPTERS.md) and a real bridge: accounts named by the
//! service, a match created and marked underway, and the result reported
//! with each game in the order played; a report delivered again, a
//! conflicting one, a bare 404, refusals and rate limits, and an account
//! nobody can be found by.
mod support;

use axum::http::StatusCode;
use ember_brackets::{Adapter, RoundRule, Rounds, Side};
use ember_protocol::EmberId;
use serde_json::{Value, json};
use support::{
    Extra, Setup, World,
    service::{FakeService, Recorded, SERVICE_TOKEN},
};

const PLAYER2_DISCORD: &str = "274220342558756145";
const BRACKET: &str = "http:pools";

fn player1_and_player2() -> Value {
    json!({
        "id": "s-1",
        "round": 1,
        "round_label": "Pools A, round 1",
        "status": "open",
        "entries": [
            { "id": "player1", "accounts": [{ "site": "challonge", "id": "Player1" }] },
            { "id": "player2", "accounts": [{ "site": "startgg", "id": "77" }, { "discord": PLAYER2_DISCORD }] },
        ],
    })
}

/// A service with Player 1 and Player 2's set, both connected in Ember: Player 1 by a
/// Challonge account, Player 2 by the Discord account the service gives as a
/// second.
async fn world(sets: Vec<Value>) -> (FakeService, World, [EmberId; 2]) {
    let service = FakeService::start(sets).await;
    let world = World::start(Setup {
        challonge: false,
        extra: vec![Extra {
            adapter: Adapter::Http,
            tournament: "pools".into(),
            api_base: service.base.clone(),
            token: SERVICE_TOKEN,
        }],
        ..Setup::default()
    })
    .await;
    let player1 = world.bridge.connect(1, "player1").await;
    let player2 = world.bridge.connect_discord(2, PLAYER2_DISCORD).await;
    (service, world, [player1, player2])
}

/// Creates s-1's match and plays it to Player 1's 2-1 win, delivered.
async fn played(service: &FakeService, world: &World) -> String {
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[0, 1, 0]).await;
    world.bridge.deliver().await;
    service.log();
    match_id
}

fn player1_wins(key: &str) -> Recorded {
    Recorded {
        set: "s-1".into(),
        key: key.into(),
        body: json!({
            "winner": "player1",
            "scores": [{ "entry": "player1", "games": 2 }, { "entry": "player2", "games": 1 }],
            "games": ["player1", "player2", "player1"],
        }),
    }
}

#[tokio::test]
async fn an_organizers_own_service_is_played_and_reported() {
    let (service, world, players) = world(vec![
        player1_and_player2(),
        json!({ "id": "s-2", "round": 2, "status": "pending" }),
        // Out of the contract's shape: left out, and the rest played.
        json!({ "id": "s-4", "round": 1, "status": "open", "entries": [] }),
        json!({ "id": "s-5", "round": 1, "status": "paused" }),
    ])
    .await;
    let [player1, player2] = players;

    world.connector.pass(true).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/underway"]
    );
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    let (external, match_id, _) = &matches[0];
    assert_eq!(external, "http:pools:s-1:1");
    let found = world.snapshot(match_id).await;
    assert_eq!(found["metadata"]["round_label"], "Pools A, round 1");
    assert_eq!(found["participants"][0]["ember_id"], player1.as_str());
    assert_eq!(found["participants"][1]["ember_id"], player2.as_str());

    // Player 1 wins 2-1. The read the result makes shows the set unchanged, so
    // the report goes straight out, keyed by the Ember match.
    world.play(match_id, &[0, 1, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/report"]
    );
    assert_eq!(service.reports(), [player1_wins(match_id)]);
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
}

#[tokio::test]
async fn a_report_delivered_twice_is_answered_as_the_first() {
    let (service, world, _) = world(vec![player1_and_player2()]).await;
    // The service records the report but its answer is lost, and its
    // reads stay cached: the set still looks open.
    service.with(|state| {
        state.lose_report_answer = true;
        state.stale_reads = true;
    });
    let match_id = played(&service, &world).await;
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/report"]
    );
    assert_eq!(world.progress(BRACKET, "s-1"), "live");

    // Sent again, with the same key and result: the service answers as it
    // did the first time, and the set is reported.
    world.advance(61);
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/report"]
    );
    assert_eq!(service.reports(), [player1_wins(&match_id)]);
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
}

#[tokio::test]
async fn a_set_with_a_different_result_is_left_to_the_organizer() {
    let (service, world, _) = world(vec![player1_and_player2()]).await;
    played(&service, &world).await;
    // The organizer entered Player 2's win, which the cached reads do not show
    // yet.
    let theirs = Recorded {
        key: "entered-by-hand".into(),
        body: json!({ "winner": "player2" }),
        ..player1_wins("")
    };
    service.with(|state| {
        state.stale_reads = true;
        state.reports.push(theirs.clone());
    });
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/report"]
    );
    assert_eq!(world.progress(BRACKET, "s-1"), "stuck");
    assert_eq!(service.reports(), [theirs]);

    // Nothing is sent again.
    world.advance(601);
    world.connector.pass(false).await;
    assert!(
        service
            .log()
            .iter()
            .all(|line| line != "POST /svc/sets/s-1/report")
    );
}

#[tokio::test]
async fn only_a_set_the_service_says_is_gone_has_its_match_cancelled() {
    let (service, world, _) = world(vec![player1_and_player2()]).await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    service.log();

    // A redeployed service answers a bare 404 for the set: no evidence.
    service.with(|state| state.hidden.insert("s-1".into()));
    world.advance(601);
    world.connector.pass(true).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "GET /svc/sets/s-1"]
    );
    assert_eq!(world.progress(BRACKET, "s-1"), "live");
    assert_ne!(world.snapshot(&match_id).await["state"], "cancelled");

    // The service says in so many words that it has no such set.
    service.with(|state| {
        state.hidden.clear();
        state.sets.clear();
    });
    world.advance(61);
    world.connector.pass(true).await;
    // With no set left open, the service is asked whether it is over; it
    // has no `/standing`, so it is running.
    assert_eq!(
        service.log(),
        [
            "GET /svc/sets?state=open",
            "GET /svc/sets/s-1",
            "GET /svc/standing"
        ]
    );
    assert_eq!(world.snapshot(&match_id).await["state"], "cancelled");
    // Gone before it was decided: played again if it ever opens again.
    assert_eq!(world.progress(BRACKET, "s-1"), "waiting");
}

#[tokio::test]
async fn a_refused_token_and_a_rate_limit_are_waited_out() {
    let (service, world, _) = world(vec![player1_and_player2()]).await;
    service.with(|state| {
        state
            .fail_next
            .extend([StatusCode::UNAUTHORIZED, StatusCode::TOO_MANY_REQUESTS])
    });
    world.connector.pass(true).await;
    // Nothing is read again until the wait is over.
    world.advance(30);
    world.connector.pass(true).await;
    assert_eq!(service.log(), ["GET /svc/sets?state=open"]);
    world.advance(31);
    world.connector.pass(true).await;
    assert_eq!(service.log(), ["GET /svc/sets?state=open"]);
    assert!(world.bridge.matches().await.is_empty());

    // The wait doubles, then the read works.
    world.advance(121);
    world.connector.pass(true).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/underway"]
    );
    assert_eq!(world.bridge.matches().await.len(), 1);
}

#[tokio::test]
async fn an_account_nobody_can_be_found_by_holds_only_its_own_set() {
    let (service, world, _) = world(vec![
        player1_and_player2(),
        // A start.gg username where start.gg needs a user ID, which finds
        // nobody, and a site the bridge does not have, which is not asked.
        json!({
            "id": "s-3",
            "round": 1,
            "status": "open",
            "entries": [
                { "id": "player3", "accounts": [{ "site": "startgg", "id": "player3" }] },
                { "id": "player4", "accounts": [{ "site": "smashgg", "id": "12" }] },
            ],
        }),
    ])
    .await;
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    assert_eq!(matches[0].0, "http:pools:s-1:1");
    assert_eq!(world.progress(BRACKET, "s-3"), "waiting");
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/underway"]
    );
}

#[tokio::test]
async fn a_report_the_service_defers_waits_alone_and_is_sent_again() {
    let (service, world, _) = world(vec![player1_and_player2()]).await;
    let match_id = played(&service, &world).await;
    service.with(|state| {
        state
            .deferred_reports
            .insert("s-1".into(), "the scorer is busy".into())
    });
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/report"]
    );
    assert_eq!(world.progress(BRACKET, "s-1"), "live");

    // The tournament is still read while the result waits its minute.
    world.advance(30);
    world.connector.pass(true).await;
    assert_eq!(service.log(), ["GET /svc/sets?state=open"]);

    service.with(|state| state.deferred_reports.clear());
    world.advance(31);
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/report"]
    );
    assert_eq!(service.reports(), [player1_wins(&match_id)]);
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
}

#[tokio::test]
async fn any_2xx_answer_to_a_write_is_taken() {
    let (service, world, _) = world(vec![player1_and_player2()]).await;
    // A service that answers its writes with `200 OK, recorded`.
    service.with(|state| state.text_answers = true);
    let match_id = played(&service, &world).await;
    assert!(service.with(|state| state.sets[0]["underway"] == true));
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/report"]
    );
    assert_eq!(service.reports(), [player1_wins(&match_id)]);
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
}

#[tokio::test]
async fn a_contract_mismatch_holds_results_until_it_is_fixed() {
    let (service, world, _) = world(vec![player1_and_player2()]).await;
    let match_id = played(&service, &world).await;
    // The service moved to another contract version for a moment.
    service.with(|state| state.fail_reports.push_back(StatusCode::UPGRADE_REQUIRED));
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/report"]
    );
    assert_eq!(world.progress(BRACKET, "s-1"), "live");
    assert!(service.reports().is_empty());

    world.advance(61);
    world.connector.pass(false).await;
    assert_eq!(service.reports(), [player1_wins(&match_id)]);
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
}

/// Another set, s-3, for two more players, with Challonge accounts.
fn player5_and_player6() -> Value {
    json!({
        "id": "s-3",
        "round": 1,
        "status": "open",
        "entries": [
            { "id": "player5", "accounts": [{ "site": "challonge", "id": "player5" }] },
            { "id": "player6", "accounts": [{ "site": "challonge", "id": "player6" }] },
        ],
    })
}

#[tokio::test]
async fn a_report_that_fails_for_a_moment_waits_alone() {
    let mut second = player1_and_player2();
    second["id"] = json!("s-2");
    second["entries"][0] =
        json!({ "id": "player3", "accounts": [{ "site": "challonge", "id": "player3" }] });
    second["entries"][1] =
        json!({ "id": "player4", "accounts": [{ "site": "challonge", "id": "player4" }] });
    let (service, world, _) = world(vec![player1_and_player2(), second]).await;
    for (byte, name) in [
        (3, "player3"),
        (4, "player4"),
        (5, "player5"),
        (6, "player6"),
    ] {
        world.bridge.connect(byte, name).await;
    }
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    // s-1's result arrives first.
    for set in ["s-1", "s-2"] {
        let (_, match_id, _) = matches
            .iter()
            .find(|(external, _, _)| external.ends_with(&format!(":{set}:1")))
            .unwrap();
        world.play(match_id, &[0, 0]).await;
        world.bridge.deliver().await;
        world.advance(1);
    }
    // The service fails on s-1's report alone, and s-3 opens.
    service.with(|state| {
        state
            .fail_reports
            .push_back(StatusCode::SERVICE_UNAVAILABLE);
        state.sets.push(player5_and_player6());
    });
    service.log();
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        [
            "GET /svc/sets?state=open",
            "POST /svc/sets/s-1/report",
            // s-1's report may have gone in: s-2 is read again first.
            "GET /svc/sets/s-2",
            "POST /svc/sets/s-2/report",
            "POST /svc/sets/s-3/underway",
        ]
    );
    assert_eq!(world.progress(BRACKET, "s-1"), "live");
    assert_eq!(world.progress(BRACKET, "s-2"), "reported");
    assert_eq!(world.bridge.matches().await.len(), 3);

    // A minute on, s-1's report is sent again, and taken.
    world.advance(61);
    world.connector.pass(false).await;
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
}

#[tokio::test]
async fn a_result_that_cannot_be_settled_does_not_stop_new_matches() {
    let (service, world, _) = world(vec![player1_and_player2()]).await;
    world.bridge.connect(5, "player5").await;
    world.bridge.connect(6, "player6").await;
    played(&service, &world).await;
    // The list leaves s-1 out, and asked alone it answers a bare 404: the
    // result cannot be settled now. s-3 opens meanwhile.
    service.with(|state| {
        state.hidden.insert("s-1".into());
        state.sets.push(player5_and_player6());
    });
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        [
            "GET /svc/sets?state=open",
            // For the result, then for its live match.
            "GET /svc/sets/s-1",
            "GET /svc/sets/s-1",
            "POST /svc/sets/s-3/underway",
        ]
    );
    assert_eq!(world.progress(BRACKET, "s-1"), "live");
    assert_eq!(world.bridge.matches().await.len(), 2);
}

#[tokio::test]
async fn a_token_refused_on_a_report_holds_the_whole_tournament() {
    let (service, world, _) = world(vec![player1_and_player2()]).await;
    let match_id = played(&service, &world).await;
    // A token the service takes for reads but refuses for writes: no one
    // set is at fault, so none waits on its own or is given up.
    service.with(|state| state.fail_reports.push_back(StatusCode::UNAUTHORIZED));
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/report"]
    );
    assert_eq!(world.progress(BRACKET, "s-1"), "live");
    // The whole tournament waits out the growing wait: not even a forced
    // read is made.
    world.advance(30);
    world.connector.pass(true).await;
    assert!(service.log().is_empty());
    world.advance(31);
    world.connector.pass(false).await;
    assert_eq!(service.reports(), [player1_wins(&match_id)]);
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
}

#[tokio::test]
async fn a_refused_token_on_one_report_still_tries_the_others() {
    let mut second = player1_and_player2();
    second["id"] = json!("s-2");
    second["entries"][0] =
        json!({ "id": "player3", "accounts": [{ "site": "challonge", "id": "player3" }] });
    second["entries"][1] =
        json!({ "id": "player4", "accounts": [{ "site": "challonge", "id": "player4" }] });
    let (service, world, _) = world(vec![player1_and_player2(), second]).await;
    world.bridge.connect(3, "player3").await;
    world.bridge.connect(4, "player4").await;
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    for set in ["s-1", "s-2"] {
        let (_, match_id, _) = matches
            .iter()
            .find(|(external, _, _)| external.ends_with(&format!(":{set}:1")))
            .unwrap();
        world.play(match_id, &[0, 0]).await;
        world.bridge.deliver().await;
        world.advance(1);
    }
    service.log();
    // s-1's report is refused for the token, s-2's goes in: nothing waits
    // behind the refusal, and nothing is given up for it.
    service.with(|state| state.fail_reports.push_back(StatusCode::FORBIDDEN));
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        [
            "GET /svc/sets?state=open",
            "POST /svc/sets/s-1/report",
            "POST /svc/sets/s-2/report",
        ]
    );
    assert_eq!(world.progress(BRACKET, "s-1"), "live");
    assert_eq!(world.progress(BRACKET, "s-2"), "reported");
    // The whole tournament waits out the refusal, then s-1 goes in.
    world.advance(30);
    world.connector.pass(true).await;
    assert!(service.log().is_empty());
    world.advance(31);
    world.connector.pass(false).await;
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
}

#[tokio::test]
async fn a_set_s_phase_names_the_rule_for_its_length() {
    // Two round 1 sets, one of pools and one of top 8, where top 8 is
    // first to 3.
    let mut pools = player1_and_player2();
    pools["phase"] = json!("Pools");
    let mut top = player1_and_player2();
    top["id"] = json!("s-2");
    top["phase"] = json!("Top 8");
    top["entries"][0] =
        json!({ "id": "player3", "accounts": [{ "site": "challonge", "id": "player3" }] });
    top["entries"][1] =
        json!({ "id": "player4", "accounts": [{ "site": "challonge", "id": "player4" }] });
    let service = FakeService::start(vec![pools, top]).await;
    let world = World::start(Setup {
        challonge: false,
        extra: vec![Extra {
            adapter: Adapter::Http,
            tournament: "pools".into(),
            api_base: service.base.clone(),
            token: SERVICE_TOKEN,
        }],
        rounds: Rounds {
            games_to_win: 2,
            rules: vec![RoundRule {
                phase: Some("Top 8".into()),
                from_round: 1,
                to_round: None,
                side: Side::Any,
                games_to_win: 3,
            }],
        },
        ..Setup::default()
    })
    .await;
    world.bridge.connect(1, "player1").await;
    world.bridge.connect_discord(2, PLAYER2_DISCORD).await;
    world.bridge.connect(3, "player3").await;
    world.bridge.connect(4, "player4").await;
    world.connector.pass(true).await;
    let mut lengths = Vec::new();
    for (external, match_id, _) in world.bridge.matches().await {
        lengths.push((
            external,
            world.snapshot(&match_id).await["games_to_win"].clone(),
        ));
    }
    lengths.sort_by(|a, b| a.0.cmp(&b.0));
    assert_eq!(
        lengths,
        [
            ("http:pools:s-1:1".to_owned(), json!(2)),
            ("http:pools:s-2:1".to_owned(), json!(3)),
        ]
    );
}
