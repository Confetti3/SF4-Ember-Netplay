//! The start.gg adapter between a fake start.gg and a real bridge: players
//! found by their start.gg account or the Discord account they linked
//! there, sets started and reported game by game, a set the organizer
//! decided never written over, and a Challonge and a start.gg tournament
//! run by one connector.
mod support;

use ember_brackets::Adapter;
use serde_json::json;
use support::{
    Extra, Setup, World,
    challonge::lines,
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
