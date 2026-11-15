//! An organizer's own event, read from the bridge's desk connection: run
//! with the token the bridge hands out, which never reaches the connector's
//! database, and no longer read once the organizer stops it.
mod support;

use serde_json::json;
use support::{Setup, World, service::SERVICE_TOKEN};

#[tokio::test]
async fn the_desks_event_is_played_and_stops_when_its_organizer_stops_it() {
    let world = World::start(Setup {
        configured: false,
        desk: true,
        ..Setup::default()
    })
    .await;
    world.bridge.connect(1).await;
    world.bridge.connect(2).await;

    // Nothing runs before the organizer starts the event.
    world.connector.pass(true).await;
    assert!(world.service.log().is_empty());

    world
        .run_event(json!({ "rounds": { "games_to_win": 3 } }), false)
        .await;
    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    assert_eq!(matches[0].0, "http:weekly:s-1:1");
    assert_eq!(world.snapshot(&matches[0].1).await["games_to_win"], 3);
    // The token stays in memory.
    let database = std::fs::read(&world.config.database).unwrap();
    assert!(
        !database
            .windows(SERVICE_TOKEN.len())
            .any(|window| window == SERVICE_TOKEN.as_bytes())
    );

    // Stopped on the bridge: the bracket is read no more.
    world
        .bridge
        .state()
        .db
        .write(|tx| {
            tx.execute("UPDATE organizer_events SET ended_at = 1", [])?;
            Ok(())
        })
        .await
        .unwrap();
    world.service.log();
    world.connector.pass(true).await;
    assert!(world.service.log().is_empty());
}

#[tokio::test]
async fn an_event_a_configured_tournament_runs_is_left_to_it() {
    let world = World::start(Setup {
        desk: true,
        ..Setup::default()
    })
    .await;
    world.bridge.connect(1).await;
    world.bridge.connect(2).await;
    world
        .run_event(json!({ "rounds": { "games_to_win": 3 } }), false)
        .await;

    world.connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    // The configured tournament's length, not the event's.
    assert_eq!(world.snapshot(&matches[0].1).await["games_to_win"], 2);
}
