//! Players signing up through Ember for an organizer's event that Ember
//! created on Challonge: each is added to the Challonge event under their
//! Challonge username, once, and only while sign-ups are open.
mod common;
#[allow(dead_code, unused_imports)]
#[path = "challonge/support.rs"]
mod support;

use common::{sign_in, sites::bridge_with_desk};
use ember_protocol::challenge::{Action, Method};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use support::*;

#[tokio::test]
async fn players_sign_up_through_ember_and_join_the_challonge_event() {
    let fake = fake_challonge();
    fake.lists("/v2.1/tournaments.json", |_| json!({ "data": [] }));
    // The stand-in creates at its listing's path; the participants requests
    // are pointed there too, so its answer tells the two apart.
    fake.creates(|_, sent| match sent["data"]["type"].as_str() {
        Some("participant") => json!({ "data": { "id": "355", "type": "participant",
            "attributes": { "name": sent["data"]["attributes"]["name"] } } }),
        _ => json!({ "data": { "id": "777", "type": "tournament",
            "attributes": { "name": "Cup", "url": "ember_cup" } } }),
    });
    let origin = fake.serve().await;
    let mut site = challonge_site(&origin);
    site.limits = Some(LIMITS);
    let participants = site
        .organizer
        .as_mut()
        .and_then(|organizer| organizer.participants.as_mut())
        .unwrap();
    participants.url = format!("{origin}/v2.1/tournaments.json?event={{tournament}}");
    participants.remove_url =
        format!("{origin}/v2.1/tournaments.json?event={{tournament}}&remove={{participant}}");
    let bridge = bridge_with_desk(
        vec![site],
        with_secret(),
        vec![direct(BRACKETS, false, &[SITE])],
        Some(BRACKETS),
    )
    .await;
    let organizer = player(&bridge, 1).await;
    let player2 = player(&bridge, 2).await;
    let player3 = player(&bridge, 3).await;
    CHALLONGE
        .organize(&bridge, &organizer, "good-player1")
        .await;
    CHALLONGE.connect(&bridge, &player2, "good-player2").await;
    let send = |who, action, method, path: String| {
        let bridge = &bridge;
        async move { sign_in::send(bridge, who, action, method, &path, json!({})).await }
    };
    let change = |command: Json, path: String| {
        let (bridge, organizer) = (&bridge, &organizer);
        async move {
            sign_in::send(
                bridge,
                organizer,
                Action::SiteConnect,
                Method::Post,
                &path,
                command,
            )
            .await
        }
    };
    let (_, made) = change(json!({ "title": "Cup" }), "/v1/organizer/drafts".into()).await;
    let id = made["id"].as_str().unwrap().to_owned();
    let event = |rest: &str| format!("/v1/organizer/events/{id}{rest}");
    let sign_up = format!("/v1/events/{id}/sign-up");
    let listed = || async {
        bridge
            .get(organizer.token(), "/v1/organizer/events")
            .await
            .1
    };

    // Only an event on a site that takes participants opens sign-ups.
    let (status, _) = change(json!({ "sign_ups": true }), event("")).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = change(json!({ "site": SITE }), event("/create")).await;
    assert_eq!(status, StatusCode::CREATED);
    let (status, _) = send(&player2, Action::SiteConnect, Method::Post, sign_up.clone()).await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "closed until opened");
    let (status, _) = change(json!({ "sign_ups": true }), event("")).await;
    assert_eq!(status, StatusCode::OK);

    // A player signs up and is added under their Challonge username, once.
    let (status, signed) = send(&player2, Action::SiteConnect, Method::Post, sign_up.clone()).await;
    assert_eq!(status, StatusCode::CREATED, "{signed}");
    let added = fake.seen("/v2.1/tournaments.json").pop().unwrap();
    assert_eq!(added.query["event"], "777");
    let sent: Json = serde_json::from_str(&added.body).unwrap();
    let attributes = &sent["data"]["attributes"];
    assert_eq!(attributes["name"], "player2");
    assert_eq!(attributes["username"], "player2");
    assert_eq!(
        attributes["misc"],
        format!("ember:{}", player2.id().as_str())
    );
    let posts = || {
        fake.seen("/v2.1/tournaments.json")
            .iter()
            .filter(|seen| seen.query.contains_key("event"))
            .count()
    };
    let (status, _) = send(&player2, Action::SiteConnect, Method::Post, sign_up.clone()).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(posts(), 1);
    let event_listed = &listed().await["events"][0];
    assert_eq!(event_listed["sign_ups"], true);
    assert_eq!(event_listed["signed_up"], 1);

    // Without a Challonge account in Ember there is no one to add.
    let (status, _) = send(&player3, Action::SiteConnect, Method::Post, sign_up.clone()).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);

    // A withdrawal Challonge does not confirm keeps the sign-up.
    // ponytail: the stand-in has no DELETE route, so only the kept sign-up
    // is checked here; the removal itself needs one there.
    let (status, _) = send(
        &player2,
        Action::SiteRemove,
        Method::Delete,
        sign_up.clone(),
    )
    .await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
    assert_eq!(listed().await["events"][0]["signed_up"], 1);

    // Ready closes sign-ups.
    let starts_at = bridge.clock.now() + 300;
    let (status, _) = change(json!({ "starts_at": starts_at }), event("")).await;
    assert_eq!(status, StatusCode::OK);
    let (status, _) = change(json!({}), event("/ready")).await;
    assert_eq!(status, StatusCode::OK);
    CHALLONGE.connect(&bridge, &player3, "good-player3").await;
    let (status, _) = send(&player3, Action::SiteConnect, Method::Post, sign_up.clone()).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = send(&player2, Action::SiteRemove, Method::Delete, sign_up).await;
    assert_eq!(
        status,
        StatusCode::BAD_REQUEST,
        "started events keep their players"
    );
}
