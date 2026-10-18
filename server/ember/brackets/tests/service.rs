//! The `http` adapter between an organizer's own service (a fake answering
//! the contract in ADAPTERS.md) and a real bridge: accounts named by the
//! service, a match created and marked underway, and the result reported
//! with each game in the order played.
mod support;

use ember_brackets::Adapter;
use serde_json::json;
use support::{
    Extra, Setup, World,
    service::{FakeService, SERVICE_TOKEN},
};

const SAM_DISCORD: &str = "274220342558756145";

#[tokio::test]
async fn an_organizers_own_service_is_played_and_reported() {
    let service = FakeService::start(vec![
        json!({
            "id": "s-1",
            "round": 1,
            "round_label": "Pools A, round 1",
            "status": "open",
            "entries": [
                { "id": "kate", "accounts": [{ "site": "challonge", "id": "Kate" }] },
                { "id": "sam", "accounts": [{ "site": "startgg", "id": "77" }, { "discord": SAM_DISCORD }] },
            ],
        }),
        json!({ "id": "s-2", "round": 2, "status": "pending" }),
    ])
    .await;
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
    let kate = world.bridge.connect(1, "kate").await;
    // Sam has no start.gg account in Ember: the service's second account
    // for him, his Discord, finds him.
    let sam = world.bridge.connect_discord(2, SAM_DISCORD).await;

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
    assert_eq!(found["participants"][0]["ember_id"], kate.as_str());
    assert_eq!(found["participants"][1]["ember_id"], sam.as_str());

    // Kate wins 2-1. The read the result makes shows the set unchanged, so
    // the report goes straight out.
    world.play(match_id, &[0, 1, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/report"]
    );
    let reports = service.state.lock().unwrap().reports.clone();
    assert_eq!(
        reports,
        [(
            "s-1".to_owned(),
            json!({
                "winner": "kate",
                "scores": [{ "entry": "kate", "games": 2 }, { "entry": "sam", "games": 1 }],
                "games": ["kate", "sam", "kate"],
            })
        )]
    );
}
