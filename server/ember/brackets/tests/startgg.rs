//! The start.gg adapter between a fake start.gg and a real bridge: players
//! found by their start.gg account or the Discord account they linked
//! there, sets started and reported game by game, a set the organizer
//! decided never written over, a report start.gg refuses sent again while
//! the set stays open, a completed event, games that do not add up,
//! entrants read again, and a Challonge and a start.gg tournament run by
//! one connector.
mod support;

use axum::http::StatusCode;
use ember_brackets::Adapter;
use ember_protocol::{partner::result_id, webhook};
use serde_json::{Value, json};
use support::{
    CONNECTION, Extra, Setup, World,
    challonge::lines,
    result_secret,
    startgg::{EVENT_SLUG, FakeEntrant, FakeSet, FakeStartgg, STARTGG_TOKEN},
};

const KATE_USER: u64 = 5_001;
const SAM_DISCORD: &str = "274220342558756145";

/// Kate's entrant (2401411) has her start.gg account connected in Ember;
/// Sam's (2401412) is found by the Discord account he linked on start.gg.
/// They play set 65089253; set 65089254 waits on another entrant.
async fn fake() -> FakeStartgg {
    FakeStartgg::start(
        vec![
            FakeEntrant {
                id: 2_401_411,
                user: Some(KATE_USER),
                discord: None,
            },
            FakeEntrant {
                id: 2_401_412,
                user: Some(5_002),
                discord: Some(SAM_DISCORD.into()),
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

fn startgg(fake: &FakeStartgg) -> Extra {
    Extra {
        adapter: Adapter::Startgg,
        tournament: EVENT_SLUG.into(),
        api_base: fake.origin.clone(),
        token: STARTGG_TOKEN,
    }
}

async fn world(fake: &FakeStartgg, challonge: bool) -> World {
    let world = World::start(Setup {
        challonge,
        extra: vec![startgg(fake)],
        ..Setup::default()
    })
    .await;
    world.bridge.connect_startgg(1, KATE_USER).await;
    world.bridge.connect_discord(2, SAM_DISCORD).await;
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

    // Sam wins 2-1, losing the second game.
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
    world.bridge.connect(3, "Kate").await;
    world.bridge.connect(4, "sam_99").await;

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

/// Creates set 65089253's match and plays it to Sam's 2-1 win, delivered.
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
                user: Some(KATE_USER),
                discord: None,
            },
            FakeEntrant {
                id: 2_401_412,
                user: Some(5_002),
                discord: Some(SAM_DISCORD.into()),
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
    let world = world(&fake, false).await;
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
    // Sam has not linked Discord on start.gg yet.
    fake.state.lock().unwrap().entrants[1].discord = None;
    let world = world(&fake, false).await;
    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Event", "Sets", "Entrants"]);
    assert!(world.bridge.matches().await.is_empty());

    fake.state.lock().unwrap().entrants[1].discord = Some(SAM_DISCORD.into());
    world.advance(600);
    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Sets"]);
    // A quarter of an hour after the last read of them.
    world.advance(300);
    world.connector.pass(true).await;
    assert_eq!(fake.operations(), ["Sets", "Entrants", "Underway"]);
    assert_eq!(world.bridge.matches().await.len(), 1);
}
