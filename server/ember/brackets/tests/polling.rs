//! How often the connector asks Challonge: outside a tournament's window
//! only to report results, never after it has finished, less often while
//! nothing changes, and not past the monthly budget except to report.
mod support;

use support::{Setup, World, challonge::lines};

const LIST: &str = "GET /matches.json?state=open&page=1&per_page=25";
const PARTICIPANTS: &str = "GET /participants.json?page=1&per_page=25";
const UNDERWAY: &str = "PUT /matches/1001/change_state.json";
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

/// The requests a pass makes after the clock moves on by `secs`.
async fn after(world: &World, secs: i64) -> Vec<String> {
    world.clock.advance(secs);
    world.connector.pass(false).await;
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
    world.connector.pass(true).await;
    assert!(world.challonge.log().is_empty());
    assert!(world.bridge.matches().await.is_empty());

    // Both clocks move on together, so the bridge's signed results stay
    // recent for the connector.
    let advance = |secs: i64| {
        world.clock.advance(secs);
        world.bridge.clock.advance(secs);
    };
    advance((FROM - world.clock.now()) as i64);
    world.connector.pass(true).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, PARTICIPANTS, UNDERWAY]
    );
    let match_id = world.bridge.matches().await[0].1.clone();

    // After the window nothing is read on a schedule.
    advance(3 * 60 * 60);
    world.connector.pass(true).await;
    assert!(world.challonge.log().is_empty());

    // A result that arrives is still read for and reported. 1002's players
    // have connected meanwhile, but no match is created any more.
    world.bridge.connect(3, "ryu").await;
    world.bridge.connect(4, "ken").await;
    world.play(&match_id, &[0, 0]).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, "PUT /matches/1001.json"]
    );
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("355"));
    assert_eq!(world.bridge.matches().await.len(), 1);
}

#[tokio::test]
async fn quiet_reads_back_off_and_a_change_resets_them() {
    let world = connected(Setup::default()).await;
    world.connector.pass(true).await;
    world.challonge.log();

    // Each read that finds the same open sets doubles the wait, from 120
    // seconds up to 600. Marking 1001 underway is not a change. Each step
    // checks 30 seconds early too, clear of the wall clock moving on.
    for wait in [120, 240, 480, 600, 600] {
        assert!(after(&world, wait - 30).await.is_empty(), "{wait}");
        assert_eq!(after(&world, 30).await, [LIST], "{wait}");
    }

    // The organizer decides 1002: the next read finds it gone, so the one
    // after comes 120 seconds later.
    world.challonge.decide("1002", 0);
    assert!(after(&world, 570).await.is_empty());
    assert_eq!(after(&world, 30).await, [LIST]);
    assert!(after(&world, 90).await.is_empty());
    assert_eq!(after(&world, 30).await, [LIST]);
    assert!(after(&world, 210).await.is_empty());
    assert_eq!(after(&world, 30).await, [LIST]);
}

#[tokio::test]
async fn a_finished_bracket_is_not_read_again_even_after_a_restart() {
    let world = World::start(Setup::default()).await;
    world.connector.pass(true).await;
    world.challonge.decide("1001", 0);
    world.challonge.decide("1002", 0);
    world.connector.pass(true).await;
    world.challonge.decide("1003", 1);
    world.challonge.log();

    // No set is open, the tournament has no state to say, some sets are
    // decided and none waits: it is finished.
    world.connector.pass(true).await;
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, TOURNAMENT, ANY_COMPLETE, ANY_PENDING]
    );
    world.connector.pass(true).await;
    world.restarted().pass(true).await;
    assert!(world.challonge.log().is_empty());
}

#[tokio::test]
async fn a_bracket_that_has_not_started_is_not_finished() {
    let world = World::start(Setup {
        matches: Vec::new(),
        ..Setup::default()
    })
    .await;
    for _ in 0..2 {
        world.connector.pass(true).await;
        assert_eq!(
            lines(&world.challonge.log()),
            [LIST, TOURNAMENT, ANY_COMPLETE]
        );
    }

    // When Challonge gives the tournament's state, that decides.
    world.challonge.state.lock().unwrap().tournament_state = Some("underway");
    world.connector.pass(true).await;
    assert_eq!(lines(&world.challonge.log()), [LIST, TOURNAMENT]);
    world.challonge.state.lock().unwrap().tournament_state = Some("complete");
    world.connector.pass(true).await;
    assert_eq!(lines(&world.challonge.log()), [LIST, TOURNAMENT]);
    world.connector.pass(true).await;
    assert!(world.challonge.log().is_empty());
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
    assert_eq!(
        lines(&world.challonge.log()),
        [LIST, "PUT /matches/1001.json"]
    );
    world.connector.pass(true).await;
    assert!(world.challonge.log().is_empty());
    assert_eq!(world.challonge.find("1001").winner.as_deref(), Some("354"));

    // Next month the bracket is read again.
    world.clock.advance(32 * 24 * 60 * 60);
    world.connector.pass(true).await;
    assert_eq!(lines(&world.challonge.log()), [LIST]);
}
