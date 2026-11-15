//! How often the connector asks the bracket service: outside a tournament's
//! window only to report results, never after the service says the
//! tournament is over, less often while nothing changes, and after a
//! failure only once a growing wait has passed.
mod support;

use axum::http::StatusCode;
use support::{Setup, World};

const LIST: &str = "GET /sets?state=open";
const UNDERWAY: &str = "POST /sets/s-1/underway";
const REPORT: &str = "POST /sets/s-1/report";
const STANDING: &str = "GET /standing";
/// 2030-01-01T00:00:00Z.
const FROM: u64 = 1_893_456_000;

async fn connected(setup: Setup) -> World {
    let world = World::start(setup).await;
    world.bridge.connect(1).await;
    world.bridge.connect(2).await;
    world
}

/// The requests a pass makes after both clocks move on by `secs`.
async fn after(world: &World, secs: i64) -> Vec<String> {
    world.advance(secs);
    world.connector.pass(false).await;
    world.service.log()
}

/// The requests a forced read makes.
async fn read(world: &World) -> Vec<String> {
    world.connector.pass(true).await;
    world.service.log()
}

fn winner(world: &World) -> serde_json::Value {
    world
        .service
        .with(|state| state.set("s-1")["winner"].clone())
}

#[tokio::test]
async fn outside_the_window_only_results_are_read_for_and_reported() {
    let world = connected(Setup {
        active_from: Some("2030-01-01T00:00:00Z".into()),
        active_until: Some("2030-01-01T03:00:00Z".into()),
        ..Setup::default()
    })
    .await;
    assert!(read(&world).await.is_empty());
    assert!(world.bridge.matches().await.is_empty());

    world.advance((FROM - world.clock.now()) as i64);
    assert_eq!(read(&world).await, [LIST, UNDERWAY]);
    let match_id = world.bridge.matches().await[0].1.clone();

    // After the window nothing is read on a schedule.
    world.advance(3 * 60 * 60);
    assert!(read(&world).await.is_empty());

    // A result that arrives is still read for and reported. s-2's players
    // have connected meanwhile, but no match is created any more.
    world.bridge.connect(3).await;
    world.bridge.connect(4).await;
    world.play(&match_id, &[0, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(world.service.log(), [LIST, REPORT]);
    assert_eq!(winner(&world), "player1");
    assert_eq!(world.bridge.matches().await.len(), 1);
}

#[tokio::test]
async fn quiet_reads_back_off_and_a_change_resets_them() {
    let world = connected(Setup::default()).await;
    world.connector.pass(true).await;
    world.service.log();

    // Each read that finds the same open sets doubles the wait, from 120
    // seconds up to 600. Marking s-1 underway is not a change.
    for wait in [120, 240, 480, 600, 600] {
        assert!(after(&world, wait - 1).await.is_empty(), "{wait}");
        assert_eq!(after(&world, 1).await, [LIST], "{wait}");
    }

    // The organizer decides s-2: the next read finds it gone, so the one
    // after comes 120 seconds later, then 240.
    world.service.with(|state| state.decide("s-2", 0));
    assert!(after(&world, 599).await.is_empty());
    assert_eq!(after(&world, 1).await, [LIST]);
    assert!(after(&world, 119).await.is_empty());
    assert_eq!(after(&world, 1).await, [LIST]);
    assert!(after(&world, 239).await.is_empty());
    assert_eq!(after(&world, 1).await, [LIST]);
}

#[tokio::test]
async fn a_failing_service_is_asked_again_after_a_growing_wait() {
    let world = connected(Setup::default()).await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[1, 1]).await;
    world.bridge.deliver().await;
    world.service.log();

    // The result waits while the service fails its next five requests.
    // Each wait doubles from a minute, and neither the result nor the
    // schedule (due every 120 to 600 seconds) reads before a wait is over.
    // Checked minute by minute for half an hour.
    world.service.with(|state| {
        state
            .fail_next
            .extend([StatusCode::INTERNAL_SERVER_ERROR; 5])
    });
    let mut reads = Vec::new();
    for minute in 0..=30 {
        let requests = after(&world, if minute == 0 { 0 } else { 60 }).await;
        if !requests.is_empty() {
            assert_eq!(requests, [LIST], "{minute}");
            reads.push(minute * 60);
        }
    }
    assert_eq!(reads, [0, 60, 180, 420, 900]);
    // The next try, 960 seconds after the last, works.
    assert_eq!(after(&world, 60).await, [LIST, REPORT]);
    assert_eq!(winner(&world), "player2");
    // Settled: nothing more until the schedule says so.
    assert!(after(&world, 0).await.is_empty());
}

#[tokio::test]
async fn only_a_finished_tournament_stops_reads_for_good() {
    let world = World::start(Setup::default()).await;
    world.connector.pass(true).await;
    world.service.with(|state| {
        state.decide("s-1", 0);
        state.decide("s-2", 0);
        state.decide("s-3", 1);
    });
    world.service.log();

    // A service without `/standing` is running: asked again each read.
    assert_eq!(read(&world).await, [LIST, STANDING]);
    assert_eq!(read(&world).await, [LIST, STANDING]);

    // Every set played only slows reads down: it is not asked again, the
    // next read is ten minutes away, and a restart reads as before.
    world
        .service
        .with(|state| state.standing = Some("played_out"));
    assert_eq!(read(&world).await, [LIST, STANDING]);
    assert!(after(&world, 599).await.is_empty());
    assert_eq!(after(&world, 1).await, [LIST]);
    world.restarted().pass(true).await;
    assert_eq!(world.service.log(), [LIST, STANDING]);

    // The service saying it is over stops reads for good, across restarts.
    world
        .service
        .with(|state| state.standing = Some("finished"));
    world.restarted().pass(true).await;
    assert_eq!(world.service.log(), [LIST, STANDING]);
    world.restarted().pass(true).await;
    assert!(world.service.log().is_empty());
}

#[tokio::test]
async fn a_bracket_that_has_not_started_is_not_asked_whether_it_is_over() {
    let world = World::start(Setup {
        sets: Vec::new(),
        ..Setup::default()
    })
    .await;
    for _ in 0..2 {
        assert_eq!(read(&world).await, [LIST]);
    }
}
