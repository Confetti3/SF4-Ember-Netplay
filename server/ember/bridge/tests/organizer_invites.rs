//! Tournament Mode by invitation (`desk.invite_only`): only an Ember ID that
//! redeemed a single-use invite gets organizer access, the web desk and
//! the desk's actions; everyone else is told how to get in.
mod common;
#[allow(dead_code, unused_imports)]
#[path = "challonge/support.rs"]
mod support;

use common::{Bridge, sign_in};
use ember_protocol::challenge::{Action, Method};
use reqwest::StatusCode;
use serde_json::json;
use support::*;

#[tokio::test]
async fn only_an_invited_organizer_runs_tournaments() {
    let fake = fake_challonge();
    let mut site = challonge_site(&fake.serve().await);
    site.limits = Some(common::sites::LIMITS);
    let bridge = Bridge::start_with(
        |config| {
            config.desk = Some(ember_bridge::config::Desk {
                connection: BRACKETS.into(),
                invite_only: true,
            });
            config.sign_in_sites = vec![site];
            config.integration_secrets = Some("unused-in-tests.json".into());
            config.tenants.push(ember_bridge::config::Tenant {
                id: "cups".into(),
                name: "Cups".into(),
                connections: vec![common::sites::direct(BRACKETS, false, &[SITE])],
            });
        },
        with_secret(),
    )
    .await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let access = |token: String| {
        let bridge = &bridge;
        async move { bridge.get(&token, "/v1/organizer/access").await.1 }
    };
    let redeem = |player: &'static str, code: String| {
        let bridge = &bridge;
        let who = if player == "1" { &player1 } else { &player2 };
        async move {
            sign_in::send(
                bridge,
                who,
                Action::SiteConnect,
                Method::Post,
                "/v1/organizer/invites/redeem",
                json!({ "code": code }),
            )
            .await
        }
    };

    // Not invited: told so, and no organizer sign-in, draft or desk.
    assert_eq!(
        access(player1.token().to_owned()).await,
        json!({ "invite_only": true, "allowed": false })
    );
    let organize = ember_protocol::sites::organize_path(SITE);
    let (status, refused) = sign_in::send(
        &bridge,
        &player1,
        Action::SiteConnect,
        Method::Post,
        &organize,
        json!({}),
    )
    .await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    assert_eq!(refused["error"]["details"]["reason"], "invite_required");
    let (status, _) = sign_in::send(
        &bridge,
        &player1,
        Action::SiteConnect,
        Method::Post,
        "/v1/organizer/drafts",
        json!({ "title": "Weekly" }),
    )
    .await;
    assert_eq!(status, StatusCode::FORBIDDEN);

    // The operator's invite works once, for whoever redeems it first.
    let codes = ember_bridge::make_invites(bridge.state(), 1, Some("a streamer".into()))
        .await
        .unwrap();
    let (status, _) = redeem("1", "0000000000".into()).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, answer) = redeem("1", codes[0].to_lowercase()).await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    assert_eq!(access(player1.token().to_owned()).await["allowed"], true);
    let (status, _) = redeem("2", codes[0].clone()).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    assert_eq!(access(player2.token().to_owned()).await["allowed"], false);

    // Now an organizer: drafts and organizer sign-in go through.
    let (status, made) = sign_in::send(
        &bridge,
        &player1,
        Action::SiteConnect,
        Method::Post,
        "/v1/organizer/drafts",
        json!({ "title": "Weekly" }),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED, "{made}");
    CHALLONGE.start_organizing(&bridge, &player1).await;
    let listed = ember_bridge::list_organizers(bridge.state()).await.unwrap();
    assert_eq!(listed.len(), 1);
    assert_eq!(listed[0].2.as_deref(), Some("a streamer"));

    // The operator can also vet an organizer directly.
    assert!(
        ember_bridge::add_organizer(bridge.state(), player2.id().as_str(), Some("known".into()))
            .await
            .unwrap()
    );
    assert_eq!(access(player2.token().to_owned()).await["allowed"], true);
    assert!(
        !ember_bridge::add_organizer(bridge.state(), player2.id().as_str(), None)
            .await
            .unwrap()
    );

    // Taken off the list, they start nothing new.
    assert!(
        ember_bridge::remove_organizer(bridge.state(), player1.id().as_str())
            .await
            .unwrap()
    );
    let (status, _) = sign_in::send(
        &bridge,
        &player1,
        Action::SiteConnect,
        Method::Post,
        "/v1/organizer/drafts",
        json!({ "title": "Again" }),
    )
    .await;
    assert_eq!(status, StatusCode::FORBIDDEN);
}
