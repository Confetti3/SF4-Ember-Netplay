//! A desk event's sets: sent to the bridge after each read, served with
//! each Ember match's score and state, to anyone only while the organizer
//! makes the event public.
mod support;

use reqwest::{Method, StatusCode};
use serde_json::json;
use support::{Setup, World};

/// A world whose desk runs the fake service's tournament, made public with
/// `settings`, and players 1 and 2 connected.
async fn public_event(settings: serde_json::Value) -> World {
    let world = World::start(Setup {
        configured: false,
        desk: true,
        ..Setup::default()
    })
    .await;
    world.bridge.connect(1).await;
    world.bridge.connect(2).await;
    world.run_event(settings, true).await;
    world
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
    let world = public_event(json!({ "rounds": { "games_to_win": 2 } })).await;

    // Read: the set's match is made, and the set is called.
    world.connector.pass(true).await;
    let match_id = world.bridge.matches().await[0].1.clone();
    let (status, set) = public_set(&world).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(set["id"], "s-1");
    assert_eq!(set["round_label"], "Round 1");
    assert_eq!(set["side"], "winners");
    assert_eq!(set["desk_state"], "called");
    assert_eq!(set["score"], json!([0, 0]));
    // Ember IDs and match IDs stay with the organizer.
    assert!(set.get("ember_ids").is_none() && set.get("match_id").is_none());

    // Played: the connector reports its score to the bracket site on the
    // same pass, so the set is done.
    world.play(&match_id, &[0, 0]).await;
    world.connector.pass(true).await;
    let (_, set) = public_set(&world).await;
    assert_eq!(set["desk_state"], "done");
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
async fn a_set_held_from_the_desk_is_not_played() {
    let world =
        public_event(json!({ "rounds": { "games_to_win": 2 }, "hold": { "sets": ["s-1"] } })).await;
    world.connector.pass(true).await;
    assert!(world.bridge.matches().await.is_empty());
    let (status, set) = public_set(&world).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(set["desk_state"], "held");
    // No Ember match yet, and the public view carries none of the
    // service's IDs or accounts.
    assert!(
        set.get("entries").is_none() && set.get("names").is_none() && set.get("discord").is_none(),
        "{set}"
    );
}
