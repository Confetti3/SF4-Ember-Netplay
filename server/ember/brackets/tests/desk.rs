//! An organizer's own event, read from the bridge's desk connection: run
//! with the token the bridge hands out, which never reaches the connector's
//! database, and no longer read once the organizer stops it.
mod support;

use std::collections::BTreeMap;

use ember_brackets::{CredentialKind, Desk};
use rusqlite::params;
use serde_json::json;
use support::{CHALLONGE_KEY, Setup, TOURNAMENT, World, challonge::lines};

/// The organizer, byte 9, connects their Challonge account and runs the
/// fake tournament on the desk, as the bridge's organizer routes would
/// leave it (the bridge's own tests cover those).
async fn run_event(world: &World, settings: serde_json::Value) {
    let organizer = world.bridge.connect(9, "organizer").await;
    let state = world.bridge.state();
    let now = world.bridge.clock.now();
    let sealed = state.keys.seal(
        json!({
            "access": CHALLONGE_KEY,
            "refresh": null,
            "scope": "me tournaments:read matches:read matches:write participants:read",
            "expires_at": now + 604_800,
        })
        .to_string()
        .as_bytes(),
    );
    let ember_id = organizer.as_str().to_owned();
    state
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO organizer_grants (site, user_id, sealed, refreshed_at)
                 VALUES ('challonge', '9009', ?1, ?2)",
                params![sealed, now],
            )?;
            tx.execute(
                "INSERT INTO organizer_events
                     (id, ember_id, title, status, site, tournament, name, settings, created_at)
                 VALUES ('evt_1', ?1, 'Weekly', 'ready', 'challonge', ?2, 'Weekly', ?3, ?4)",
                params![ember_id, TOURNAMENT, settings.to_string(), now],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

#[tokio::test]
async fn the_desks_event_is_played_and_stops_when_its_organizer_stops_it() {
    let world = World::start(Setup {
        challonge: false,
        kind: CredentialKind::Bearer,
        ..Setup::default()
    })
    .await;
    let mut config = world.config.clone();
    config.desk = Some(Desk {
        api_bases: BTreeMap::from([("challonge".into(), world.challonge.api.clone())]),
        ..Desk::default()
    });
    let connector = support::connector(&config, &world.clock);
    world.bridge.connect(1, "player1").await;
    world.bridge.connect(2, "Player_2").await;

    // Nothing runs before the organizer starts the event.
    connector.pass(true).await;
    assert!(world.challonge.log().is_empty());

    run_event(&world, json!({ "rounds": { "games_to_win": 3 } })).await;
    connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    assert_eq!(matches[0].0, "challonge:ember_weekly:1001:1");
    assert_eq!(world.snapshot(&matches[0].1).await["games_to_win"], 3);
    // The token stays in memory.
    let database = std::fs::read(&config.database).unwrap();
    assert!(
        !database
            .windows(CHALLONGE_KEY.len())
            .any(|window| window == CHALLONGE_KEY.as_bytes())
    );

    // Stopped on the bridge: the bracket is read no more.
    let state = world.bridge.state();
    state
        .db
        .write(|tx| {
            tx.execute("UPDATE organizer_events SET ended_at = 1", [])?;
            Ok(())
        })
        .await
        .unwrap();
    world.challonge.log();
    connector.pass(true).await;
    assert!(lines(&world.challonge.log()).is_empty());
}

#[tokio::test]
async fn an_event_a_configured_tournament_runs_is_left_to_it() {
    let world = World::start(Setup {
        kind: CredentialKind::Bearer,
        ..Setup::default()
    })
    .await;
    let mut config = world.config.clone();
    config.desk = Some(Desk {
        api_bases: BTreeMap::from([("challonge".into(), world.challonge.api.clone())]),
        ..Desk::default()
    });
    let connector = support::connector(&config, &world.clock);
    world.bridge.connect(1, "player1").await;
    world.bridge.connect(2, "Player_2").await;
    run_event(&world, json!({ "rounds": { "games_to_win": 3 } })).await;

    connector.pass(true).await;
    let matches = world.bridge.matches().await;
    assert_eq!(matches.len(), 1);
    // The configured tournament's length, not the event's.
    assert_eq!(world.snapshot(&matches[0].1).await["games_to_win"], 2);
}
