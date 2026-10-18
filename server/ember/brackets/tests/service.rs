//! The `http` adapter between an organizer's own service (a fake answering
//! the contract in ADAPTERS.md) and a real bridge: accounts named by the
//! service, a match created and marked underway, and the result reported
//! with each game in the order played; a report delivered again, a
//! conflicting one, a bare 404, refusals and rate limits, and an account
//! the bridge will not look up.
mod support;

use axum::http::StatusCode;
use ember_brackets::Adapter;
use ember_protocol::EmberId;
use serde_json::{Value, json};
use support::{
    Extra, Setup, World,
    service::{FakeService, Recorded, SERVICE_TOKEN},
};

const SAM_DISCORD: &str = "274220342558756145";
const BRACKET: &str = "http:pools";

fn kate_and_sam() -> Value {
    json!({
        "id": "s-1",
        "round": 1,
        "round_label": "Pools A, round 1",
        "status": "open",
        "entries": [
            { "id": "kate", "accounts": [{ "site": "challonge", "id": "Kate" }] },
            { "id": "sam", "accounts": [{ "site": "startgg", "id": "77" }, { "discord": SAM_DISCORD }] },
        ],
    })
}

/// A service with Kate and Sam's set, both connected in Ember: Kate by her
/// Challonge account, Sam by the Discord account the service gives as his
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
    let kate = world.bridge.connect(1, "kate").await;
    let sam = world.bridge.connect_discord(2, SAM_DISCORD).await;
    (service, world, [kate, sam])
}

/// Creates s-1's match and plays it to Kate's 2-1 win, delivered.
async fn played(service: &FakeService, world: &World) -> String {
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[0, 1, 0]).await;
    world.bridge.deliver().await;
    service.log();
    match_id
}

fn kate_wins(key: &str) -> Recorded {
    Recorded {
        set: "s-1".into(),
        key: key.into(),
        body: json!({
            "winner": "kate",
            "scores": [{ "entry": "kate", "games": 2 }, { "entry": "sam", "games": 1 }],
            "games": ["kate", "sam", "kate"],
        }),
    }
}

#[tokio::test]
async fn an_organizers_own_service_is_played_and_reported() {
    let (service, world, players) = world(vec![
        kate_and_sam(),
        json!({ "id": "s-2", "round": 2, "status": "pending" }),
        // Out of the contract's shape: left out, and the rest played.
        json!({ "id": "s-4", "round": 1, "status": "open", "entries": [] }),
        json!({ "id": "s-5", "round": 1, "status": "paused" }),
    ])
    .await;
    let [kate, sam] = players;

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
    // the report goes straight out, keyed by the Ember match.
    world.play(match_id, &[0, 1, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(
        service.log(),
        ["GET /svc/sets?state=open", "POST /svc/sets/s-1/report"]
    );
    assert_eq!(service.reports(), [kate_wins(match_id)]);
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
}

#[tokio::test]
async fn a_report_delivered_twice_is_answered_as_the_first() {
    let (service, world, _) = world(vec![kate_and_sam()]).await;
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
    assert_eq!(service.reports(), [kate_wins(&match_id)]);
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
}

#[tokio::test]
async fn a_set_with_a_different_result_is_left_to_the_organizer() {
    let (service, world, _) = world(vec![kate_and_sam()]).await;
    played(&service, &world).await;
    // The organizer entered Sam's win, which the cached reads do not show
    // yet.
    let theirs = Recorded {
        key: "entered-by-hand".into(),
        body: json!({ "winner": "sam" }),
        ..kate_wins("")
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
    let (service, world, _) = world(vec![kate_and_sam()]).await;
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
    assert_eq!(world.progress(BRACKET, "s-1"), "closed");
}

#[tokio::test]
async fn a_refused_token_and_a_rate_limit_are_waited_out() {
    let (service, world, _) = world(vec![kate_and_sam()]).await;
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
async fn an_account_the_bridge_will_not_look_up_holds_only_its_own_set() {
    let (service, world, _) = world(vec![
        kate_and_sam(),
        // A start.gg username where start.gg needs a user ID, and a site
        // the connection may not find players by.
        json!({
            "id": "s-3",
            "round": 1,
            "status": "open",
            "entries": [
                { "id": "ryu", "accounts": [{ "site": "startgg", "id": "ryu" }] },
                { "id": "ken", "accounts": [{ "site": "smashgg", "id": "12" }] },
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
    let (service, world, _) = world(vec![kate_and_sam()]).await;
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
    assert_eq!(service.reports(), [kate_wins(&match_id)]);
    assert_eq!(world.progress(BRACKET, "s-1"), "reported");
}
