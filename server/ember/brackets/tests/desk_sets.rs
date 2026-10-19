//! A desk event's sets: sent to the bridge after each read, served with
//! each Ember match's score and state, to anyone only while the organizer
//! makes the event public.
mod support;

use std::collections::BTreeMap;

use ember_brackets::{CredentialKind, Desk};
use reqwest::{Method, StatusCode};
use rusqlite::params;
use serde_json::json;
use support::{CHALLONGE_KEY, Setup, TOURNAMENT, World};

/// The organizer, byte 9, runs the fake tournament on the desk, made public.
async fn run_public_event(world: &World) {
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
                     (id, ember_id, title, status, site, tournament, name, settings, created_at,
                      public)
                 VALUES ('evt_1', ?1, 'Weekly', 'ready', 'challonge', ?2, 'Weekly', ?3, ?4, 1)",
                params![
                    ember_id,
                    TOURNAMENT,
                    json!({ "rounds": { "games_to_win": 2 } }).to_string(),
                    now
                ],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

async fn public_set(world: &World) -> (StatusCode, serde_json::Value) {
    let (status, view) = world
        .bridge
        .send("", Method::GET, "/v1/public/events/evt_1", None, None)
        .await;
    (status, view["sets"][0].clone())
}

#[tokio::test]
async fn a_desk_events_sets_follow_its_matches_and_show_while_public() {
    let world = World::start(Setup {
        challonge: false,
        kind: CredentialKind::Bearer,
        ..Setup::default()
    })
    .await;
    let mut config = world.config.clone();
    config.desk = Some(Desk {
        api_bases: BTreeMap::from([("challonge".into(), world.challonge.api.clone())]),
    });
    let connector = support::connector(&config, &world.clock);
    world.bridge.connect(1, "player1").await;
    world.bridge.connect(2, "Player_2").await;
    connector.pass(true).await;
    run_public_event(&world).await;

    // Read: the set's match is made, and the set is called.
    connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    let (status, set) = public_set(&world).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(set["id"], "1001");
    assert_eq!(set["round_label"], "Round 1");
    assert_eq!(set["side"], "winners");
    assert_eq!(set["desk_state"], "called");
    assert_eq!(set["players"], json!(["player1", "Player_2"]));
    assert_eq!(set["score"], json!([0, 0]));
    // Ember IDs and match IDs stay with the organizer.
    assert!(set.get("ember_ids").is_none() && set.get("match_id").is_none());

    // Played: its score, waiting to be reported (the results URL here is
    // another connector's; the report itself is covered in brackets.rs).
    world.play(&match_id, &[0, 0]).await;
    connector.pass(true).await;
    let (_, set) = public_set(&world).await;
    assert_eq!(set["desk_state"], "reporting");
    assert_eq!(set["score"], json!([2, 0]));

    // No longer public: nobody else reads it.
    world
        .bridge
        .state()
        .db
        .write(|tx| {
            tx.execute("UPDATE organizer_events SET public = 0", [])?;
            Ok(())
        })
        .await
        .unwrap();
    let (status, _) = public_set(&world).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
}

#[tokio::test]
async fn a_desk_events_sets_hold_the_whole_bracket() {
    let world = World::start(Setup {
        challonge: false,
        kind: CredentialKind::Bearer,
        ..Setup::default()
    })
    .await;
    // A later round nobody can play yet: never in a read of open sets.
    world
        .challonge
        .state
        .lock()
        .unwrap()
        .matches
        .push(support::challonge::FakeMatch::fed_by(
            "2001",
            2,
            ["1001", "1001"],
        ));
    let mut config = world.config.clone();
    config.desk = Some(Desk {
        api_bases: BTreeMap::from([("challonge".into(), world.challonge.api.clone())]),
    });
    let connector = support::connector(&config, &world.clock);
    world.bridge.connect(1, "player1").await;
    world.bridge.connect(2, "Player_2").await;
    connector.pass(true).await;
    run_public_event(&world).await;
    connector.pass(true).await;

    let (status, view) = world
        .bridge
        .send("", Method::GET, "/v1/public/events/evt_1", None, None)
        .await;
    assert_eq!(status, StatusCode::OK);
    let later = view["sets"]
        .as_array()
        .unwrap()
        .iter()
        .find(|set| set["id"] == "2001")
        .expect("the later round is in the bracket");
    assert_eq!(later["round"], 2);
    assert_eq!(later["desk_state"], "not_ready");
}
