//! How often the connector asks Challonge: outside a tournament's window
//! only to report results, never after the site says the tournament is over,
//! less often while nothing changes, after a failure only once a growing
//! wait has passed, and not past the monthly budget except to report.
mod support;

use support::{Setup, World, challonge::lines};

const LIST: &str = "GET /matches.json?state=open&page=1&per_page=25";
const PARTICIPANTS: &str = "GET /participants.json?page=1&per_page=25";
const UNDERWAY: &str = "PUT /matches/1001/change_state.json";
const REPORT: &str = "PUT /matches/1001.json";
const TOURNAMENT: &str = "GET .json";
const ANY_COMPLETE: &str = "GET /matches.json?state=complete&page=1&per_page=1";
const ANY_PENDING: &str = "GET /matches.json?state=pending&page=1&per_page=1";
/// 2030-01-01T00:00:00Z.
const FROM: u64 = 1_893_456_000;

async fn connected(setup: Setup) -> World {
    let world = World::start(setup).await;
    world.bridge.connect(1, "Kate").await;
    world.bridge.connect(2, "sam_99").await;
    world
}

/// The requests a pass makes after both clocks move on by `secs`.
async fn after(world: &World, secs: i64) -> Vec<String> {
    world.advance(secs);
    world.connector.pass(false).await;
    lines(&world.challonge.log())
}

/// The requests a forced read makes.
async fn read(world: &World) -> Vec<String> {
    world.connector.pass(true).await;
    lines(&world.challonge.log())
}

#[tokio::test]
async fn outside_the_window_only_results_are_read_for_and_reported() {
    let mut setup = Setup {
        active_from: Some("2030-01-01T00:00:00Z".into()),
        active_until: Some("2030-01-01T03:00:00Z".into()),
        ..Setup::default()
    };
    setup.participants[3].1 = Some("ken".into());
    let world = connected(setup).await;
    assert!(read(&world).await.is_empty());
    assert!(world.bridge.matches().await.is_empty());

    world.advance((FROM - world.clock.now()) as i64);
    assert_eq!(read(&world).await, [LIST, PARTICIPANTS, UNDERWAY]);
    let match_id = world.bridge.matches().await[0].1.clone();

    // After the window nothing is read on a schedule.
    world.advance(3 * 60 * 60);
    assert!(read(&world).await.is_empty());

    // A result that arrives is still read for and reported. 1002's players
    // have connected meanwhile, but no match is created any more.
    world.bridge.connect(3, "ryu").await;
    world.bridge.connect(4, "ken").await;
    world.play(&match_id, &[0, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(lines(&world.challonge.log()), [LIST, REPORT]);
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("355"));
    assert_eq!(world.bridge.matches().await.len(), 1);
}

#[tokio::test]
async fn quiet_reads_back_off_and_a_change_resets_them() {
    let world = connected(Setup::default()).await;
    world.connector.pass(true).await;
    world.challonge.log();

    // Each read that finds the same open sets doubles the wait, from 120
    // seconds up to 600. Marking 1001 underway is not a change.
    for wait in [120, 240, 480, 600, 600] {
        assert!(after(&world, wait - 1).await.is_empty(), "{wait}");
        assert_eq!(after(&world, 1).await, [LIST], "{wait}");
    }

    // The organizer decides 1002: the next read finds it gone, so the one
    // after comes 120 seconds later, then 240.
    world.challonge.decide("1002", 0);
    assert!(after(&world, 599).await.is_empty());
    assert_eq!(after(&world, 1).await, [LIST]);
    assert!(after(&world, 119).await.is_empty());
    assert_eq!(after(&world, 1).await, [LIST]);
    assert!(after(&world, 239).await.is_empty());
    assert_eq!(after(&world, 1).await, [LIST]);
}

#[tokio::test]
async fn a_failing_site_is_asked_again_after_a_growing_wait() {
    let world = connected(Setup::default()).await;
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[1, 1]).await;
    world.bridge.deliver().await;
    world.challonge.log();

    // The result waits while Challonge fails. Each wait doubles from a
    // minute, and neither the result nor the schedule (due every 120 to 600
    // seconds) reads before a wait is over. Checked minute by minute for
    // half an hour.
    world.challonge.state.lock().unwrap().failing = Some(500);
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
    world.challonge.state.lock().unwrap().failing = None;
    assert_eq!(after(&world, 60).await, [LIST, REPORT]);
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("354"));
    // Settled: nothing more until the schedule says so.
    assert!(after(&world, 0).await.is_empty());
}

#[tokio::test]
async fn only_an_over_tournament_stops_reads_for_good() {
    let world = World::start(Setup::default()).await;
    world.connector.pass(true).await;
    world.challonge.decide("1001", 0);
    world.challonge.decide("1002", 0);
    world.connector.pass(true).await;
    world.challonge.decide("1003", 1);
    world.challonge.log();

    // A finished group stage, with the final stage not made yet, is not over.
    world.challonge.state.lock().unwrap().tournament_state = Some("group_stages_finalized");
    assert_eq!(read(&world).await, [LIST, TOURNAMENT]);
    assert_eq!(read(&world).await, [LIST, TOURNAMENT]);

    // Without a state, every set looking played only slows reads down: it
    // is not asked again, the next read is ten minutes away, and a restart
    // reads as before.
    world.challonge.state.lock().unwrap().tournament_state = None;
    assert_eq!(
        read(&world).await,
        [LIST, TOURNAMENT, ANY_COMPLETE, ANY_PENDING]
    );
    assert!(after(&world, 599).await.is_empty());
    assert_eq!(after(&world, 1).await, [LIST]);
    world.restarted().pass(true).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, TOURNAMENT, ANY_COMPLETE, ANY_PENDING]
    );

    // Awaiting review, a set can still be reopened: that too only slows
    // reads, and is not saved.
    world.challonge.state.lock().unwrap().tournament_state = Some("awaiting_review");
    for _ in 0..2 {
        world.restarted().pass(true).await;
        assert_eq!(lines(&world.challonge.log()), [LIST, TOURNAMENT]);
    }

    // The site saying it is over stops reads for good, across restarts.
    world.challonge.state.lock().unwrap().tournament_state = Some("complete");
    world.restarted().pass(true).await;
    assert_eq!(lines(&world.challonge.log()), [LIST, TOURNAMENT]);
    world.restarted().pass(true).await;
    assert!(world.challonge.log().is_empty());
}

#[tokio::test]
async fn a_bracket_that_has_not_started_is_not_asked_whether_it_is_over() {
    let world = World::start(Setup {
        matches: Vec::new(),
        ..Setup::default()
    })
    .await;
    for _ in 0..2 {
        assert_eq!(read(&world).await, [LIST]);
    }
}

#[tokio::test]
async fn a_spent_budget_stops_reading_brackets_but_not_reporting() {
    let world = connected(Setup {
        budget: 4,
        ..Setup::default()
    })
    .await;
    world.connector.pass(true).await;
    assert_eq!(world.challonge.log().len(), 3);
    let match_id = world.bridge.matches().await[0].1.clone();
    world.play(&match_id, &[1, 1]).await;
    world.bridge.deliver().await;

    // The fourth and fifth requests read for the result and report it.
    world.connector.pass(false).await;
    assert_eq!(lines(&world.challonge.log()), [LIST, REPORT]);
    assert!(read(&world).await.is_empty());
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("354"));

    // Next month the bracket is read again.
    world.advance(32 * 24 * 60 * 60);
    assert_eq!(read(&world).await, [LIST]);
}
