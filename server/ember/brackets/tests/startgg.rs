//! The start.gg adapter between a fake start.gg and a real bridge: players
//! found by their start.gg account or the Discord account they linked
//! there, sets started and reported game by game, a set the organizer
//! decided never written over, a report start.gg refuses sent again while
//! the set stays open, a completed event, games that do not add up,
//! entrants read again, and a Challonge and a start.gg tournament run by
//! one connector.
mod support;

use axum::http::StatusCode;
use ember_protocol::{partner::result_id, webhook};
use serde_json::{Value, json};
use support::{
    CONNECTION, Setup, World,
    challonge::lines,
    result_secret,
    startgg::{EVENT_SLUG, FakeEntrant, FakeSet, FakeStartgg, PLAYER1_USER, extra},
};

const PLAYER2_DISCORD: &str = "274220342558756145";

/// Player 1's entrant (2401411) has its start.gg account connected in Ember;
/// Player 2's (2401412) is found by the Discord account linked on start.gg.
/// They play set 65089253; set 65089254 waits on another entrant.
async fn fake() -> FakeStartgg {
    FakeStartgg::start(
        vec![
            FakeEntrant {
                id: 2_401_411,
                user: Some(PLAYER1_USER),
                discord: None,
            },
            FakeEntrant {
                id: 2_401_412,
                user: Some(5_002),
                discord: Some(PLAYER2_DISCORD.into()),
            },
            FakeEntrant {
                id: 2_401_413,
                user: None,
                discord: None,
            },
        ],
        vec![
            FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412]),
            FakeSet::open(65_089_254, 1, [2_401_413, 2_401_411]),
        ],
    )
    .await
}

async fn world(fake: &FakeStartgg, challonge: bool) -> World {
    world_serving(fake, challonge, true).await
}

/// `world`, whose connector answers on its results URL only when
/// `serve_results`. Without it the bridge's own delivery of a finished match
/// fails instead of reaching the connector, so a test that hands the
/// connector a result of its own is not raced by the real one.
async fn world_serving(fake: &FakeStartgg, challonge: bool, serve_results: bool) -> World {
    let world = World::start(Setup {
        challonge,
        extra: vec![extra(fake)],
        serve_results,
        ..Setup::default()
    })
    .await;
    world.bridge.connect_startgg(1, PLAYER1_USER).await;
    world.bridge.connect_discord(2, PLAYER2_DISCORD).await;
    world
}

#[tokio::test]
async fn a_set_is_found_by_startgg_and_discord_accounts_and_reported_game_by_game() {
    let fake = fake().await;
    let world = world(&fake, false).await;

    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Event", "Sets", "Entrants", "Underway"]);
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    let (external, match_id, _) = &matches[0];
    assert_eq!(external, &format!("startgg:{EVENT_SLUG}:65089253:1"));
    let found = world.snapshot(match_id).await;
    assert_eq!(found["metadata"]["round_label"], "Winners Round 1");
    let players: Vec<_> = found["participants"]
        .as_array()
        .unwrap()
        .iter()
        .map(|participant| participant["slot"].clone())
        .collect();
    assert_eq!(players, [json!(0), json!(1)]);
    assert_eq!(fake.set(65_089_253).state, 2);

    // Player 2 wins 2-1, losing the second game.
    world.play(match_id, &[1, 0, 1]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    let log = fake.log();
    let operations: Vec<&str> = log.iter().map(|logged| logged.operation.as_str()).collect();
    assert_eq!(operations, ["Sets", "Set", "Report"]);
    assert_eq!(
        log[2].variables,
        json!({
            "setId": "65089253",
            "winnerId": "2401412",
            "gameData": [
                { "gameNum": 1, "winnerId": "2401412" },
                { "gameNum": 2, "winnerId": "2401411" },
                { "gameNum": 3, "winnerId": "2401412" },
            ],
        })
    );
    let set = fake.set(65_089_253);
    assert_eq!((set.state, set.winner), (3, Some(2_401_412)));
    assert_eq!(set.games, [2_401_412, 2_401_411, 2_401_412]);
}

#[tokio::test]
async fn a_set_the_organizer_decided_meanwhile_is_not_written_over() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[1, 1]).await;
    world.bridge.deliver().await;
    fake.log();

    // The organizer reports it on start.gg just before the connector does.
    fake.state.lock().unwrap().decide_on_reread = Some(65_089_253);
    world.connector.pass(false).await;
    assert_eq!(fake.operations(), ["Sets", "Set"]);
    let set = fake.set(65_089_253);
    assert_eq!((set.winner, set.games.len()), (Some(2_401_411), 0));
}

#[tokio::test]
async fn long_lists_are_read_page_by_page() {
    let entrants: Vec<FakeEntrant> = (0..90)
        .map(|n| FakeEntrant {
            id: 3_000_000 + n,
            user: None,
            discord: None,
        })
        .collect();
    let sets = (0..45)
        .map(|n| FakeSet::open(70_000_000 + n, 1, [3_000_000 + 2 * n, 3_000_001 + 2 * n]))
        .collect();
    let fake = FakeStartgg::start(entrants, sets).await;
    let world = world(&fake, false).await;
    world.connector.pass(true).await;
    let pages: Vec<(String, u64)> = fake
        .log()
        .into_iter()
        .map(|logged| {
            let page = logged.variables["page"].as_u64().unwrap_or(0);
            (logged.operation, page)
        })
        .collect();
    let expected = [
        ("Event", 0),
        ("Sets", 1),
        ("Sets", 2),
        ("Entrants", 1),
        ("Entrants", 2),
        ("Entrants", 3),
    ]
    .map(|(operation, page)| (operation.to_owned(), page));
    assert_eq!(pages, expected);
    // Nobody here has an account to find, so nothing is created.
    assert!(world.bridge.matches().await.is_empty());
}

#[tokio::test]
async fn one_connector_runs_a_challonge_and_a_startgg_tournament() {
    let fake = fake().await;
    let world = world(&fake, true).await;
    world.bridge.connect(3, "Player1").await;
    world.bridge.connect(4, "player_2").await;

    world.connector.pass(true).await;
    let mut externals: Vec<String> = world
        .bridge
        .matches()
        .await
        .into_iter()
        .map(|(external, _, _)| external)
        .collect();
    externals.sort();
    assert_eq!(
        externals,
        [
            "challonge:ember_weekly:1001:1".to_owned(),
            format!("startgg:{EVENT_SLUG}:65089253:1"),
        ]
    );

    // Each result goes back to its own site.
    let matches = world.bridge.matches().await;
    let challonge = &matches
        .iter()
        .find(|(external, _, _)| external.starts_with("challonge:"))
        .unwrap()
        .1;
    world.challonge.log();
    fake.log();
    world.play(challonge, &[0, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [
            "GET /matches.json?state=open&page=1&per_page=25",
            "PUT /matches/1001.json"
        ]
    );
    assert!(
        fake.operations()
            .iter()
            .all(|operation| operation != "Report")
    );
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("355"));
}

const BRACKET: &str = "startgg:tournament/ember-weekly/event/usf4";

/// Creates set 65089253's match and plays it to Player 2's 2-1 win, delivered.
async fn played(fake: &FakeStartgg, world: &World) -> String {
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[1, 0, 1]).await;
    world.bridge.deliver().await;
    fake.log();
    match_id
}

#[tokio::test]
async fn a_report_start_gg_refuses_while_the_set_stays_open_is_sent_again() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    fake.state.lock().unwrap().report_errors.extend([
        // An expired token, then a refusal start.gg does not explain.
        (
            StatusCode::BAD_REQUEST,
            "Invalid authentication token".into(),
        ),
        (StatusCode::OK, "Something went wrong".into()),
    ]);
    played(&fake, &world).await;
    world.connector.pass(false).await;
    assert_eq!(fake.operations(), ["Sets", "Set", "Report"]);
    world.advance(61);
    world.connector.pass(false).await;
    // Read again after the refusal: still open, so the score waits.
    assert_eq!(fake.operations(), ["Sets", "Set", "Report", "Set"]);
    assert_eq!(world.progress(BRACKET, "65089253"), "live");

    world.advance(121);
    world.connector.pass(false).await;
    assert_eq!(fake.operations(), ["Sets", "Set", "Report"]);
    let set = fake.set(65_089_253);
    assert_eq!((set.state, set.winner), (3, Some(2_401_412)));
    assert_eq!(world.progress(BRACKET, "65089253"), "reported");
}

#[tokio::test]
async fn a_report_refused_again_and_again_is_left_to_the_organizer() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    fake.state
        .lock()
        .unwrap()
        .report_errors
        .extend(std::iter::repeat_n((StatusCode::OK, "Nope".to_owned()), 20));
    played(&fake, &world).await;
    let mut reports = 0;
    while world.progress(BRACKET, "65089253") == "live" && reports < 20 {
        world.connector.pass(false).await;
        reports += fake
            .operations()
            .iter()
            .filter(|operation| *operation == "Report")
            .count();
        world.advance(3_601);
    }
    assert_eq!(reports, 12);
    assert_eq!(world.progress(BRACKET, "65089253"), "stuck");
    world.connector.pass(false).await;
    assert!(
        fake.operations()
            .iter()
            .all(|operation| operation != "Report")
    );
    assert_eq!(fake.set(65_089_253).winner, None);
}

#[tokio::test]
async fn a_set_its_players_started_meanwhile_is_still_played() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    // Started on start.gg between the read and the connector marking it:
    // start.gg answers "Set is already started", which is no failure.
    fake.state.lock().unwrap().start_after_list = Some(65_089_253);
    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Event", "Sets", "Entrants", "Underway"]);
    assert_eq!(world.progress(BRACKET, "65089253"), "live");
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[0, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(fake.operations(), ["Sets", "Set", "Report"]);
    assert_eq!(world.progress(BRACKET, "65089253"), "reported");
}

#[tokio::test]
async fn a_completed_event_is_not_read_again() {
    let fake = FakeStartgg::start(
        vec![
            FakeEntrant {
                id: 2_401_411,
                user: Some(PLAYER1_USER),
                discord: None,
            },
            FakeEntrant {
                id: 2_401_412,
                user: Some(5_002),
                discord: Some(PLAYER2_DISCORD.into()),
            },
        ],
        vec![FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412])],
    )
    .await;
    let world = world(&fake, false).await;
    played(&fake, &world).await;
    world.connector.pass(false).await;
    assert_eq!(fake.operations(), ["Sets", "Set", "Report"]);
    // No set is open now, and start.gg says the event is over.
    world.advance(601);
    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Sets", "Event"]);
    world.advance(3_600);
    world.connector.pass(true).await;
    assert!(fake.operations().is_empty());
}

#[tokio::test]
async fn games_that_do_not_add_up_to_the_score_are_not_reported() {
    let fake = fake().await;
    // The result below is the test's own, under the id the bridge's would
    // have: the bridge's must not reach the connector first.
    let world = world_serving(&fake, false, false).await;

    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[1, 0, 1]).await;
    fake.log();
    // A result that says 2-0 for a match whose games went 2-1.
    let found = world.snapshot(&match_id).await;
    let participants: Vec<Value> = found["participants"]
        .as_array()
        .unwrap()
        .iter()
        .map(|seat| {
            json!({
                "participant_id": seat["participant_id"],
                "ember_id": seat["ember_id"],
                "slot": seat["slot"],
                "score": if seat["slot"] == 1 { 2 } else { 0 },
            })
        })
        .collect();
    let winner = participants[1]["participant_id"].clone();
    let body = json!({
        "type": "io.ember.tournament.match.result.v1",
        "bridge_id": "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11",
        "connection_id": CONNECTION,
        "match_id": match_id,
        "external_match_id": format!("startgg:{EVENT_SLUG}:65089253:1"),
        "outcome": "completed",
        "revision": "9",
        "participants": participants,
        "winner_participant_id": winner,
    });
    let bytes = serde_json::to_vec(&body).unwrap();
    let headers = webhook::sign(
        &[&result_secret()],
        &result_id(&match_id),
        world.clock.now(),
        &bytes,
    )
    .unwrap();
    assert_eq!(world.connector.accept(&headers, &bytes), Ok(true));
    world.connector.pass(false).await;
    assert_eq!(fake.operations(), ["Sets"]);
    assert_eq!(world.progress(BRACKET, "65089253"), "stuck");
}

#[tokio::test]
async fn entrants_are_read_again_for_accounts_linked_since() {
    let fake = fake().await;
    // Player 2 has not linked Discord on start.gg yet.
    fake.state.lock().unwrap().entrants[1].discord = None;
    let world = world(&fake, false).await;
    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Event", "Sets", "Entrants"]);
    assert!(world.bridge.matches().await.is_empty());

    fake.state.lock().unwrap().entrants[1].discord = Some(PLAYER2_DISCORD.into());
    world.advance(600);
    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Sets"]);
    // A quarter of an hour after the last read of them.
    world.advance(300);
    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Sets", "Entrants", "Underway"]);
    assert_eq!(world.bridge.matches().await.len(), 1);
}

#[tokio::test]
async fn a_set_whose_report_is_refused_waits_alone() {
    let entrant = |id: u64, user: u64| FakeEntrant {
        id,
        user: Some(user),
        discord: None,
    };
    let fake = FakeStartgg::start(
        vec![
            entrant(2_401_411, PLAYER1_USER),
            FakeEntrant {
                id: 2_401_412,
                user: Some(5_002),
                discord: Some(PLAYER2_DISCORD.into()),
            },
            entrant(2_401_414, 5_003),
            entrant(2_401_415, 5_004),
        ],
        vec![
            FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412]),
            FakeSet::open(65_089_255, 1, [2_401_414, 2_401_415]),
        ],
    )
    .await;
    let world = world(&fake, false).await;
    world.bridge.connect_startgg(3, 5_003).await;
    world.bridge.connect_startgg(4, 5_004).await;
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 2);
    // The refused set's result arrives first, so it is settled first.
    for set in ["65089253", "65089255"] {
        let (_, match_id, _) = matches
            .iter()
            .find(|(external, _, _)| external.contains(set))
            .unwrap();
        world.play(match_id, &[0, 0]).await;
        world.bridge.deliver().await;
        world.advance(1);
    }
    // start.gg refuses the first set's score, for a reason of its own.
    fake.state
        .lock()
        .unwrap()
        .refused_sets
        .insert(65_089_253, "Something about this set".into());
    fake.log();

    // The other set's result is reported in the same pass.
    world.connector.pass(false).await;
    assert_eq!(world.progress(BRACKET, "65089255"), "reported");
    assert_eq!(world.progress(BRACKET, "65089253"), "live");
    assert_eq!(fake.set(65_089_255).winner, Some(2_401_414));
    let reports = |operations: Vec<String>| {
        operations
            .iter()
            .filter(|operation| *operation == "Report")
            .count()
    };
    assert_eq!(reports(fake.operations()), 2);

    // The tournament goes on reading; the refused result waits its minute.
    world.advance(30);
    world.connector.pass(true).await;
    let operations = fake.operations();
    assert!(operations.iter().any(|operation| operation == "Sets"));
    assert_eq!(reports(operations), 0);
    fake.state.lock().unwrap().refused_sets.clear();
    world.advance(31);
    world.connector.pass(false).await;
    assert_eq!(reports(fake.operations()), 1);
    assert_eq!(world.progress(BRACKET, "65089253"), "reported");
    assert_eq!(fake.set(65_089_253).winner, Some(2_401_411));
}

#[tokio::test]
async fn retrying_a_deferred_result_does_not_quicken_the_schedule() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    fake.state
        .lock()
        .unwrap()
        .refused_sets
        .insert(65_089_253, "Something about this set".into());
    played(&fake, &world).await;
    // Deferred four times: waits of 1, 2, 4 and 8 minutes. Nothing on
    // start.gg changes, so the schedule backs off to its longest
    // interval (600 s) meanwhile.
    let mut wait = 60;
    for _ in 0..4 {
        world.connector.pass(false).await;
        world.advance(wait);
        wait *= 2;
    }
    // The fifth try, then a 16-minute wait for the sixth.
    world.connector.pass(false).await;
    assert_eq!(world.progress(BRACKET, "65089253"), "live");
    fake.log();
    let mut reads = 0;
    for _ in 0..31 {
        world.advance(30);
        world.connector.pass(false).await;
        reads += fake
            .operations()
            .iter()
            .filter(|operation| *operation == "Sets")
            .count();
    }
    // One scheduled read in 930 s, at the longest interval. Were the
    // retry a change, reads would come after 120, 240 and 480 s again.
    assert_eq!(reads, 1);
}

#[tokio::test]
async fn a_set_start_gg_no_longer_has_has_its_match_cancelled() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    fake.log();

    // The organizer resets the phase group: start.gg answers the old set's
    // ID with "Set not found".
    fake.state.lock().unwrap().gone_sets.insert(65_089_253);
    world.advance(601);
    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Sets", "Set"]);
    assert_eq!(world.snapshot(&match_id).await["state"], "cancelled");
    assert_eq!(world.progress(BRACKET, "65089253"), "waiting");

    // And the tournament is read as before, its entrants included, a
    // quarter of an hour after they were last read.
    world.advance(601);
    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Sets", "Entrants"]);
}

#[tokio::test]
async fn private_discord_accounts_still_leave_start_gg_accounts_to_find() {
    let fake = fake().await;
    fake.state.lock().unwrap().private_discord = true;
    let world = world(&fake, false).await;
    // Player 2 can only be found by a start.gg account now.
    world.bridge.connect_startgg(3, 5_002).await;
    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Event", "Sets", "Entrants", "Underway"]);
    assert_eq!(world.bridge.matches().await.len(), 1);
}

#[tokio::test]
async fn a_connection_that_may_not_look_up_discord_still_finds_start_gg_players() {
    let entrant = |id: u64, user: u64, discord: Option<&str>| FakeEntrant {
        id,
        user: Some(user),
        discord: discord.map(str::to_owned),
    };
    let fake = FakeStartgg::start(
        vec![
            entrant(2_401_411, PLAYER1_USER, None),
            entrant(2_401_412, 5_002, Some(PLAYER2_DISCORD)),
            entrant(2_401_414, 5_003, None),
            entrant(2_401_415, 5_004, None),
        ],
        vec![
            FakeSet::open(65_089_253, 1, [2_401_411, 2_401_412]),
            FakeSet::open(65_089_255, 1, [2_401_414, 2_401_415]),
        ],
    )
    .await;
    // The bridge connection does not look players up by Discord account,
    // and Player 2 can only be found by one.
    let world = World::start(Setup {
        challonge: false,
        extra: vec![extra(&fake)],
        discord_lookup: false,
        ..Setup::default()
    })
    .await;
    world.bridge.connect_startgg(1, PLAYER1_USER).await;
    world.bridge.connect_discord(2, PLAYER2_DISCORD).await;
    world.bridge.connect_startgg(3, 5_003).await;
    world.bridge.connect_startgg(4, 5_004).await;
    world.connector.pass(true).await;
    // Only the set whose players need a Discord lookup waits.
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    assert_eq!(matches[0].0, format!("startgg:{EVENT_SLUG}:65089255:1"));
    assert_eq!(world.progress(BRACKET, "65089253"), "waiting");
}

#[tokio::test]
async fn games_the_organizer_entered_that_differ_are_not_written_over() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    played(&fake, &world).await;
    // The organizer entered game 1 for Player 1; in Ember, Player 2 won it.
    fake.state.lock().unwrap().sets[0].games = vec![2_401_411];
    world.connector.pass(false).await;
    assert_eq!(fake.operations(), ["Sets", "Set"]);
    assert_eq!(world.progress(BRACKET, "65089253"), "stuck");
    let set = fake.set(65_089_253);
    assert_eq!((set.winner, set.games), (None, vec![2_401_411]));
}

#[tokio::test]
async fn games_the_organizer_entered_that_agree_are_kept() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    played(&fake, &world).await;
    // Game 1 entered for Player 2, as in Ember: the report sends every game.
    fake.state.lock().unwrap().sets[0].games = vec![2_401_412];
    world.connector.pass(false).await;
    assert_eq!(fake.operations(), ["Sets", "Set", "Report"]);
    assert_eq!(world.progress(BRACKET, "65089253"), "reported");
    let set = fake.set(65_089_253);
    assert_eq!(set.winner, Some(2_401_412));
    assert_eq!(set.games, [2_401_412, 2_401_411, 2_401_412]);
}

#[tokio::test]
async fn a_report_whose_answer_does_not_show_it_is_checked() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    played(&fake, &world).await;
    // No errors, and no sign of the score: no answer, no sets, another
    // set, a set still open. None was recorded, so each waits.
    fake.state.lock().unwrap().report_answers.extend([
        (json!({}), false),
        (json!({ "reportBracketSet": null }), false),
        (json!({ "reportBracketSet": [] }), false),
        (
            json!({ "reportBracketSet": [{ "id": 65089254, "state": 3, "winnerId": 2401412 }] }),
            false,
        ),
        (
            json!({ "reportBracketSet": [{ "id": 65089253, "state": 2, "winnerId": null }] }),
            false,
        ),
    ]);
    for _ in 0..5 {
        world.connector.pass(false).await;
        // The entrants are read again too, once they are a quarter of an
        // hour old.
        let operations: Vec<String> = fake
            .operations()
            .into_iter()
            .filter(|operation| operation != "Entrants")
            .collect();
        assert_eq!(operations, ["Sets", "Set", "Report", "Set"]);
        assert_eq!(world.progress(BRACKET, "65089253"), "live");
        assert_eq!(fake.set(65_089_253).winner, None);
        world.advance(3_601);
    }
    // No sign in the answer, but the score went in: the read shows it.
    fake.state
        .lock()
        .unwrap()
        .report_answers
        .push_back((json!({ "reportBracketSet": null }), true));
    world.connector.pass(false).await;
    assert_eq!(world.progress(BRACKET, "65089253"), "reported");
    assert_eq!(fake.set(65_089_253).winner, Some(2_401_412));
}

#[tokio::test]
async fn a_set_that_turns_pending_before_its_report_is_not_reported() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    played(&fake, &world).await;
    // Open in the list, pending (its entrants kept) when read again.
    fake.state.lock().unwrap().pend_on_reread = Some(65_089_253);
    world.connector.pass(false).await;
    assert_eq!(fake.operations(), ["Sets", "Set"]);
    assert_eq!(world.progress(BRACKET, "65089253"), "live");
    assert_eq!(fake.set(65_089_253).winner, None);
}

#[tokio::test]
async fn a_game_entered_with_more_than_its_winner_is_left_to_the_organizer() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    played(&fake, &world).await;
    // Game 1's winner agrees with Ember's, but the organizer also entered
    // its stage, which a report would replace.
    {
        let mut state = fake.state.lock().unwrap();
        state.sets[0].games = vec![2_401_412];
        state.sets[0].game_extra = json!({ "stage": { "id": 4 } });
    }
    world.connector.pass(false).await;
    assert_eq!(fake.operations(), ["Sets", "Set"]);
    assert_eq!(world.progress(BRACKET, "65089253"), "stuck");
    assert_eq!(fake.set(65_089_253).winner, None);
}

#[tokio::test]
async fn a_game_entered_without_a_winner_is_left_to_the_organizer() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    played(&fake, &world).await;
    {
        let mut state = fake.state.lock().unwrap();
        state.sets[0].games = vec![2_401_412];
        state.sets[0].game_extra = json!({ "winnerId": null });
    }
    world.connector.pass(false).await;
    assert_eq!(fake.operations(), ["Sets", "Set"]);
    assert_eq!(world.progress(BRACKET, "65089253"), "stuck");
}

#[tokio::test]
async fn a_game_entered_with_a_score_above_zero_is_left_to_the_organizer() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    played(&fake, &world).await;
    {
        let mut state = fake.state.lock().unwrap();
        state.sets[0].games = vec![2_401_412];
        state.sets[0].game_extra = json!({ "entrant1Score": 0, "entrant2Score": 3 });
    }
    world.connector.pass(false).await;
    assert_eq!(world.progress(BRACKET, "65089253"), "stuck");
    assert_eq!(fake.set(65_089_253).winner, None);
}

#[tokio::test]
async fn a_game_start_gg_fills_with_zero_scores_is_still_only_a_winner() {
    let fake = fake().await;
    let world = world(&fake, false).await;
    played(&fake, &world).await;
    // What start.gg may give for a game entered with only its winner:
    // scores of 0, a stage with no ID, an empty list of characters.
    {
        let mut state = fake.state.lock().unwrap();
        state.sets[0].games = vec![2_401_412];
        state.sets[0].game_extra = json!({
            "entrant1Score": 0,
            "entrant2Score": 0,
            "stage": { "id": null },
            "selections": [],
        });
    }
    world.connector.pass(false).await;
    assert_eq!(world.progress(BRACKET, "65089253"), "reported");
    assert_eq!(fake.set(65_089_253).winner, Some(2_401_412));
}
