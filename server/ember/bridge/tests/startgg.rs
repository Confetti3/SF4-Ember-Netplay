//! start.gg sign-in against a stand-in start.gg, from the shipped definition
//! (`deploy/sites/startgg.json`): a JSON token exchange, the user read by a
//! GraphQL query, numeric user IDs and lookups by them. Then start.gg and
//! Challonge on one bridge, each keeping its accounts, consent, budget,
//! sign-ins and proofs to itself; an unknown site; and a third site the
//! bridge has no code for, defined only in configuration. Discord's own
//! flow is covered by tests/discord.rs and Challonge's by tests/challonge.rs.
#[allow(dead_code, unused_imports)]
#[path = "challonge/support.rs"]
mod challonge;
mod common;
#[path = "startgg/support.rs"]
mod support;

use std::sync::Arc;

use common::{Bridge, code, sign_in};
use ember_bridge::config::{SignInSite, sites::Limits};
use ember_protocol::challenge::{Action, Method};
use reqwest::StatusCode;
use serde_json::json;
use support::*;

#[tokio::test]
async fn a_player_connects_startgg_and_platforms_find_them_by_user_id() {
    let fake = fake_startgg();
    let bridge = bridge(&fake).await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    let origin = url::Url::parse(&bridge.state().config.sign_in_sites[0].authorize_url).unwrap();
    assert_eq!(
        capabilities["sign_in_sites"],
        json!([{
            "id": "startgg", "display_name": "start.gg", "sign_in": true, "lookup_by": "user_id",
            "authorize_origin": origin.origin().ascii_serialization(),
            "authorize_path": "/oauth/authorize",
        }])
    );
    let player1 = player(&bridge, 1).await;

    let state = STARTGG.start(&bridge, &player1).await;
    assert!(
        fake.seen("/oauth/access_token").is_empty(),
        "starting asks start.gg nothing"
    );
    let page = STARTGG
        .back(&bridge, &format!("code=good-player1&state={state}"))
        .await;
    assert!(
        page.contains("Connect this start.gg account?")
            && page.contains(&player1.id().fingerprint())
            && page.contains(&format!("action=\"{CALLBACK}\"")),
        "{page}"
    );
    let page = STARTGG
        .decide(&bridge, &sign_in::confirmation(&page), "connect")
        .await;
    assert!(page.contains("start.gg connected"), "{page}");
    let (_, connected) = bridge.get(player1.token(), "/v1/sites/startgg").await;
    // The numeric ID is read as its digits; the gamer tag is the name shown.
    assert_eq!(connected["account"]["user_id"], PLAYER1.0);
    assert_eq!(connected["account"]["username"], PLAYER1.1);

    // The code is exchanged in a JSON body with the client secret and the
    // definition's extra field.
    let exchange = &fake.seen("/oauth/access_token")[0];
    assert_eq!(exchange.headers["content-type"], "application/json");
    let body: serde_json::Value = serde_json::from_str(&exchange.body).unwrap();
    assert_eq!(
        body,
        json!({
            "grant_type": "authorization_code", "code": "good-player1",
            "redirect_uri": bridge.url(CALLBACK), "client_id": CLIENT_ID,
            "client_secret": CLIENT_SECRET, "scope": "user.identity",
        })
    );
    // Who signed in is read with the definition's GraphQL query, posted with
    // the token as a bearer.
    let me = &fake.seen("/gql/alpha")[0];
    assert_eq!(content_types(&me.headers), ["application/json"]);
    assert_eq!(
        me.headers["authorization"],
        format!("Bearer {}", fake.tokens()[0])
    );
    let query: serde_json::Value = serde_json::from_str(&me.body).unwrap();
    assert_eq!(
        Some(query),
        bridge.state().config.sign_in_sites[0].user.body.clone()
    );

    // A platform finds Player 1 by start.gg user ID, and links them by it.
    assert_eq!(
        found_ids(&bridge, GG, &[PLAYER1.0]).await,
        [(PLAYER1.0.to_owned(), player1.id().to_string())]
    );
    assert_eq!(
        link_of(&bridge, GG, &player1).await,
        Some(("site".into(), format!("startgg:{}", PLAYER1.0)))
    );
    assert!(!database_holds(&bridge, "access-token").await);
    assert!(!database_holds(&bridge, "refresh-token").await);
}

#[tokio::test]
async fn a_gamer_tag_is_only_shown_so_players_may_share_one() {
    let fake = fake_startgg();
    let bridge = bridge(&fake).await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    for (who, code) in [(&player1, "good-player1"), (&player2, "good-twin")] {
        let page = STARTGG.connect(&bridge, who, code).await;
        assert!(page.contains("start.gg connected"), "{page}");
    }
    // Both keep their accounts, and neither is stored under the tag, which
    // lookups never match.
    assert_eq!(
        stored(&bridge, SITE).await,
        [
            (PLAYER1.0.to_owned(), "Player 1".to_owned(), None),
            (TWIN.0.to_owned(), "Player 1".to_owned(), None),
        ]
    );
    assert_eq!(found_ids(&bridge, GG, &[PLAYER1.0, TWIN.0]).await.len(), 2);
    assert!(found_ids(&bridge, GG, &["Player 1"]).await.is_empty());
}

#[tokio::test]
async fn a_user_without_a_tag_shows_the_id_and_an_api_error_connects_nothing() {
    let fake = fake_startgg();
    let bridge = bridge(&fake).await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let page = STARTGG.connect(&bridge, &player1, "good-nameless").await;
    assert!(
        page.contains("start.gg connected") && page.contains(NAMELESS.0),
        "{page}"
    );
    // An error answer, or one larger than the bridge reads, connects nothing.
    for code in ["good-refused", "good-huge"] {
        let page = STARTGG.connect(&bridge, &player2, code).await;
        assert!(page.contains("start.gg did not answer"), "{code}: {page}");
        assert_eq!(STARTGG.account(&bridge, &player2).await, None);
    }
    assert_eq!(
        stored(&bridge, SITE).await,
        [(NAMELESS.0.to_owned(), NAMELESS.0.to_owned(), None)]
    );
}

#[tokio::test]
async fn a_lookup_names_startgg_accounts_by_user_id() {
    let fake = fake_startgg();
    let bridge = bridge(&fake).await;
    // IDs no start.gg account can have find no one, as a Discord ID that
    // is no snowflake finds no one.
    let long = "1".repeat(65);
    let (status, answer) = lookup(
        &bridge,
        GG,
        &[(SITE, "12 34"), (SITE, ""), (SITE, &long)],
        &[],
    )
    .await;
    assert_eq!((status, answer), (StatusCode::OK, json!({ "players": [] })));
    // Connections not set to find start.gg accounts may not ask, and
    // neither may this one ask Discord's list.
    let (status, _) = lookup(&bridge, GG, &[], &["274220342558756145"]).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _) = lookup(&bridge, "mock-a", &[(SITE, PLAYER1.0)], &[]).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    // Nothing found answers no accounts at all.
    let (status, answer) = lookup(&bridge, GG, &[(SITE, PLAYER1.0)], &[]).await;
    assert_eq!((status, answer), (StatusCode::OK, json!({ "players": [] })));
}

/// Connections on the bridge with both sites: one per site and one that
/// finds players everywhere.
const EVERYWHERE: &str = "everywhere-bot";

/// A bridge with Challonge (one sign-in's budget a day) and start.gg (three
/// sign-ins' a day) against their stand-ins.
async fn both(challonge_fake: &Arc<FakeSite>, startgg_fake: &Arc<FakeSite>) -> Bridge {
    let mut challonge = challonge::challonge_site(&challonge_fake.serve().await);
    challonge.limits = Some(Limits {
        daily_request_budget: 2,
        ..LIMITS
    });
    let mut startgg = startgg_site(&startgg_fake.serve().await);
    startgg.limits = Some(Limits {
        monthly_request_budget: 10,
        daily_request_budget: 6,
        starts_per_hour: 30,
    });
    bridge_with_sites(
        vec![challonge, startgg],
        secrets(&[
            (challonge::SITE, challonge::CLIENT_SECRET),
            (SITE, CLIENT_SECRET),
        ]),
        vec![
            direct(challonge::BRACKETS, false, &[challonge::SITE]),
            direct(GG, false, &[SITE]),
            direct(EVERYWHERE, true, &[challonge::SITE, SITE]),
        ],
    )
    .await
}

#[tokio::test]
async fn challonge_and_startgg_keep_their_accounts_and_consent_apart() {
    let (challonge_fake, startgg_fake) = (challonge::fake_challonge(), fake_startgg());
    let bridge = both(&challonge_fake, &startgg_fake).await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    let listed: Vec<_> = capabilities["sign_in_sites"]
        .as_array()
        .unwrap()
        .iter()
        .map(|site| site["id"].clone())
        .collect();
    assert_eq!(listed, [json!("challonge"), json!("startgg")]);

    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    challonge::connect(&bridge, &player1, "player1").await;
    assert!(
        STARTGG
            .connect(&bridge, &player1, "good-player1")
            .await
            .contains("start.gg connected")
    );
    assert!(
        STARTGG
            .connect(&bridge, &player2, "good-twin")
            .await
            .contains("start.gg connected")
    );
    assert_eq!(
        challonge::CHALLONGE
            .account(&bridge, &player1)
            .await
            .as_deref(),
        Some(challonge::PLAYER1.0)
    );
    assert_eq!(
        STARTGG.account(&bridge, &player1).await.as_deref(),
        Some(PLAYER1.0)
    );
    assert_eq!(challonge::CHALLONGE.account(&bridge, &player2).await, None);
    // Each site's IDs are its own: Challonge's user ID is no start.gg ID,
    // and a start.gg ID is no Challonge username anyone has.
    assert!(
        found_ids(&bridge, EVERYWHERE, &[challonge::PLAYER1.0])
            .await
            .is_empty()
    );
    assert!(
        challonge::CHALLONGE
            .found(&bridge, EVERYWHERE, &[PLAYER1.0])
            .await
            .is_empty()
    );

    // Each connection asks only the lists it is set to.
    let (status, _) = lookup(&bridge, challonge::BRACKETS, &[(SITE, PLAYER1.0)], &[]).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _) = lookup(&bridge, GG, &[(challonge::SITE, challonge::PLAYER1.1)], &[]).await;
    assert_eq!(status, StatusCode::FORBIDDEN);

    // One lookup on both sites finds each account under its own site, and
    // Player 1, found on both, is one link, made by the first account asked.
    let (status, answer) = lookup(
        &bridge,
        EVERYWHERE,
        &[
            (challonge::SITE, challonge::PLAYER1.1),
            (SITE, PLAYER1.0),
            (SITE, TWIN.0),
        ],
        &[],
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    let accounts: Vec<_> = answer["accounts"]
        .as_array()
        .unwrap()
        .iter()
        .map(|account| {
            (
                account["site"].as_str().unwrap().to_owned(),
                account["user_id"].as_str().unwrap().to_owned(),
                account["ember_id"].as_str().unwrap().to_owned(),
            )
        })
        .collect();
    assert_eq!(
        accounts,
        [
            (
                "challonge".into(),
                challonge::PLAYER1.0.into(),
                player1.id().to_string()
            ),
            ("startgg".into(), PLAYER1.0.into(), player1.id().to_string()),
            ("startgg".into(), TWIN.0.into(), player2.id().to_string()),
        ]
    );
    assert_eq!(
        link_of(&bridge, EVERYWHERE, &player1).await,
        Some(("site".into(), format!("challonge:{}", challonge::PLAYER1.0)))
    );

    // Player 1 removes that link: every account they have connected withdraws
    // its consent there, so neither site links them again; Player 2's is their own.
    remove_link(&bridge, &player1, EVERYWHERE).await;
    for _ in 0..2 {
        assert!(
            challonge::CHALLONGE
                .found(&bridge, EVERYWHERE, &[challonge::PLAYER1.1])
                .await
                .is_empty()
        );
        assert!(
            found_ids(&bridge, EVERYWHERE, &[PLAYER1.0])
                .await
                .is_empty()
        );
        assert_eq!(link_of(&bridge, EVERYWHERE, &player1).await, None);
    }
    assert_eq!(found_ids(&bridge, EVERYWHERE, &[TWIN.0]).await.len(), 1);
    // A new start.gg sign-in gives back that account's consent.
    STARTGG.connect(&bridge, &player1, "good-player1").await;
    assert_eq!(found_ids(&bridge, EVERYWHERE, &[PLAYER1.0]).await.len(), 1);
    assert_eq!(
        link_of(&bridge, EVERYWHERE, &player1).await,
        Some(("site".into(), format!("startgg:{}", PLAYER1.0)))
    );

    // Disconnecting start.gg ends the links it approved and leaves their
    // Challonge account and its links elsewhere.
    challonge::CHALLONGE
        .found(&bridge, challonge::BRACKETS, &[challonge::PLAYER1.1])
        .await;
    let (status, body) = STARTGG.disconnect(&bridge, &player1).await;
    assert_eq!(status, StatusCode::OK, "{body}");
    assert_eq!(STARTGG.account(&bridge, &player1).await, None);
    assert_eq!(link_of(&bridge, EVERYWHERE, &player1).await, None);
    assert_eq!(
        challonge::CHALLONGE
            .account(&bridge, &player1)
            .await
            .as_deref(),
        Some(challonge::PLAYER1.0)
    );
    assert_eq!(
        link_of(&bridge, challonge::BRACKETS, &player1).await,
        Some(("site".into(), format!("challonge:{}", challonge::PLAYER1.0)))
    );
}

#[tokio::test]
async fn removing_a_link_withdraws_discords_consent_and_every_sites() {
    let (challonge_fake, startgg_fake) = (challonge::fake_challonge(), fake_startgg());
    let bridge = both(&challonge_fake, &startgg_fake).await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let discord = |player: &str| match player {
        "player1" => "274220342558756145",
        _ => "80351110224678912",
    };
    for (player, name, code) in [
        (&player1, "player1", "good-player1"),
        (&player2, "player2", "good-twin"),
    ] {
        connect_discord(&bridge, discord(name), player).await;
        STARTGG.connect(&bridge, player, code).await;
    }
    let found_by_discord = |id: &'static str| {
        let bridge = &bridge;
        async move {
            let (status, answer) = lookup(bridge, EVERYWHERE, &[], &[id]).await;
            assert_eq!(status, StatusCode::OK, "{answer}");
            answer["players"].as_array().unwrap().len()
        }
    };

    // Player 1's link came from their Discord account. Once they remove it,
    // their start.gg account does not link them there either.
    assert_eq!(found_by_discord(discord("player1")).await, 1);
    assert_eq!(
        link_of(&bridge, EVERYWHERE, &player1).await,
        Some(("discord".into(), format!("discord:{}", discord("player1"))))
    );
    remove_link(&bridge, &player1, EVERYWHERE).await;
    assert!(
        found_ids(&bridge, EVERYWHERE, &[PLAYER1.0])
            .await
            .is_empty()
    );
    assert_eq!(found_by_discord(discord("player1")).await, 0);
    assert_eq!(link_of(&bridge, EVERYWHERE, &player1).await, None);

    // Player 2's came from start.gg. Once they remove it, their Discord account
    // does not link them there either.
    assert_eq!(found_ids(&bridge, EVERYWHERE, &[TWIN.0]).await.len(), 1);
    remove_link(&bridge, &player2, EVERYWHERE).await;
    assert_eq!(found_by_discord(discord("player2")).await, 0);
    assert!(found_ids(&bridge, EVERYWHERE, &[TWIN.0]).await.is_empty());
    assert_eq!(link_of(&bridge, EVERYWHERE, &player2).await, None);

    // Other connections keep their consent.
    assert_eq!(found_ids(&bridge, GG, &[PLAYER1.0, TWIN.0]).await.len(), 2);
}

#[tokio::test]
async fn each_sites_budget_sign_ins_and_proofs_are_its_own() {
    let (challonge_fake, startgg_fake) = (challonge::fake_challonge(), fake_startgg());
    let bridge = both(&challonge_fake, &startgg_fake).await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);

    // A proof made for one site's route is refused on the other's, for each
    // action.
    for (action, method, made_for, sent_to) in [
        (
            Action::SiteConnect,
            Method::Post,
            "/v1/sites/startgg/start",
            "/v1/sites/challonge/start",
        ),
        (
            Action::SiteCancel,
            Method::Delete,
            "/v1/sites/challonge/start",
            "/v1/sites/startgg/start",
        ),
        (
            Action::SiteRemove,
            Method::Delete,
            "/v1/sites/challonge",
            "/v1/sites/startgg",
        ),
    ] {
        let body = bridge
            .prove(&player1, action, method, made_for, json!({}))
            .await;
        let (status, body) = bridge.send_proof(&player1, method, sent_to, body).await;
        assert_eq!(
            (status, code(&body)),
            (StatusCode::UNAUTHORIZED, "invalid_signature"),
            "{made_for} -> {sent_to}: {body}"
        );
    }

    // A Challonge sign-in's state means nothing on start.gg's callback, and
    // is still good on Challonge's.
    let state = challonge::CHALLONGE.start(&bridge, &player1).await;
    let page = STARTGG
        .back(&bridge, &format!("code=good-player1&state={state}"))
        .await;
    assert!(page.contains("Sign-in expired"), "{page}");
    assert!(startgg_fake.seen("/oauth/access_token").is_empty());
    challonge::connect_with(&bridge, &state, "player1").await;

    // That spent Challonge's day; start.gg's is untouched and still open.
    assert_eq!(challonge::CHALLONGE.requests_spent(&bridge).await, 2);
    assert_eq!(STARTGG.requests_spent(&bridge).await, 0);
    let (status, body) = challonge::CHALLONGE.try_start(&bridge, &player2).await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::SERVICE_UNAVAILABLE, "sign_in_busy"),
        "{body}"
    );
    assert!(
        STARTGG
            .connect(&bridge, &player2, "good-twin")
            .await
            .contains("start.gg connected")
    );
    assert_eq!(STARTGG.requests_spent(&bridge).await, 2);
    assert_eq!(challonge::CHALLONGE.requests_spent(&bridge).await, 2);

    assert_eq!(STARTGG.account(&bridge, &player1).await, None);
    assert_eq!(
        challonge::CHALLONGE
            .account(&bridge, &player1)
            .await
            .as_deref(),
        Some(challonge::PLAYER1.0)
    );
}

#[tokio::test]
async fn an_unknown_site_is_not_found() {
    let fake = fake_startgg();
    let bridge = bridge(&fake).await;
    let player1 = player(&bridge, 1).await;
    for site in ["challonge", "discord", "nope"] {
        let start = format!("/v1/sites/{site}/start");
        let (status, body) = send(
            &bridge,
            &player1,
            Action::SiteConnect,
            Method::Post,
            &start,
            json!({}),
        )
        .await;
        assert_eq!(
            (status, code(&body)),
            (StatusCode::NOT_FOUND, "not_found"),
            "{site}: {body}"
        );
        let (status, body) = bridge
            .get(player1.token(), &format!("/v1/sites/{site}"))
            .await;
        assert_eq!(
            (status, code(&body)),
            (StatusCode::NOT_FOUND, "not_found"),
            "{site}: {body}"
        );
        // The browser gets a page, as from every other callback, with a 404.
        let response = bridge
            .client
            .get(bridge.url(&format!("/v1/sites/{site}/callback?code=x&state=y")))
            .send()
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::NOT_FOUND, "{site}");
        let page = response.text().await.unwrap();
        assert!(
            page.contains("does not offer this sign-in"),
            "{site}: {page}"
        );
        // A lookup on a site the connection is not set to find is refused,
        // whether or not the bridge has it.
        let (status, _) = lookup(&bridge, GG, &[(site, "1")], &[]).await;
        assert_eq!(status, StatusCode::FORBIDDEN, "{site}");
    }
}

#[tokio::test]
async fn a_site_defined_only_in_configuration_signs_in_and_finds_players() {
    let fake = fake_bracketeer();
    let origin = fake.serve().await;
    let site: SignInSite = serde_json::from_value(bracketeer(&origin)).unwrap();
    let bridge = bridge_with_sites(
        vec![site],
        secrets(&[("bracketeer", "bracketeer-secret")]),
        vec![direct("hub-bot", false, &["bracketeer"])],
    )
    .await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    assert_eq!(
        capabilities["sign_in_sites"],
        json!([{
            "id": "bracketeer", "display_name": "Bracketeer", "sign_in": true,
            "lookup_by": "username", "authorize_origin": origin,
            "authorize_path": "/login/oauth/authorize",
        }])
    );
    let player1 = player(&bridge, 1).await;

    // The browser goes where the definition says, with its extra parameter
    // and no scope, since it names none.
    let url = sign_in::start(&bridge, &player1, &sign_in::Routes::site("bracketeer")).await;
    let query = sign_in::query(&url);
    assert_eq!(url.path(), "/login/oauth/authorize");
    assert_eq!(query["prompt"], "login");
    assert_eq!(query["client_id"], "bracketeer-client");
    assert!(!query.contains_key("scope"));
    let page = BRACKETEER
        .back(
            &bridge,
            &format!("code=good-player1&state={}", query["state"]),
        )
        .await;
    let page = BRACKETEER
        .decide(&bridge, &sign_in::confirmation(&page), "connect")
        .await;
    assert!(page.contains("Bracketeer connected"), "{page}");

    // The form exchange carried the definition's extra field, and whoami
    // its own header.
    let exchange = &fake.seen("/login/oauth/access_token")[0];
    assert_eq!(exchange.form["audience"], "ember");
    assert_eq!(exchange.form["client_secret"], "bracketeer-secret");
    assert_eq!(
        fake.seen("/api/v3/whoami")[0].headers["accept"],
        "application/vnd.bracketeer+json"
    );
    assert_eq!(
        BRACKETEER.account(&bridge, &player1).await.as_deref(),
        Some("b-17")
    );
    assert_eq!(BRACKETEER.requests_spent(&bridge).await, 2);

    // Lookups match the handle as the rule says: with its case.
    assert_eq!(
        BRACKETEER.found(&bridge, "hub-bot", &["Player1B"]).await,
        [("Player1B".into(), "b-17".into(), player1.id().to_string())]
    );
    assert!(
        BRACKETEER
            .found(&bridge, "hub-bot", &["player1b"])
            .await
            .is_empty()
    );
    assert!(
        BRACKETEER
            .found(&bridge, "hub-bot", &["Player1.B"])
            .await
            .is_empty()
    );
    assert_eq!(
        link_of(&bridge, "hub-bot", &player1).await,
        Some(("site".into(), "bracketeer:b-17".into()))
    );
}

/// The Content-Type headers of a request, each one sent.
fn content_types(headers: &axum::http::HeaderMap) -> Vec<&str> {
    headers
        .get_all("content-type")
        .iter()
        .map(|value| value.to_str().unwrap())
        .collect()
}

#[tokio::test]
async fn a_definitions_content_type_replaces_the_json_default() {
    let fake = fake_startgg();
    let mut site = startgg_site(&fake.serve().await);
    site.user
        .headers
        .insert("Content-Type".into(), "application/graphql+json".into());
    let bridge =
        bridge_with_sites(vec![site], with_secret(), vec![direct(GG, false, &[SITE])]).await;
    let player1 = player(&bridge, 1).await;
    let page = STARTGG.connect(&bridge, &player1, "good-player1").await;
    assert!(page.contains("start.gg connected"), "{page}");
    let me = &fake.seen("/gql/alpha")[0];
    assert_eq!(content_types(&me.headers), ["application/graphql+json"]);
}

#[tokio::test]
async fn an_account_on_a_site_taken_out_of_the_configuration_can_still_go() {
    let fake = fake_startgg();
    let mut bridge = bridge(&fake).await;
    let player1 = player(&bridge, 1).await;
    STARTGG.connect(&bridge, &player1, "good-player1").await;
    assert_eq!(found_ids(&bridge, GG, &[PLAYER1.0]).await.len(), 1);

    // The operator takes start.gg out of the configuration.
    bridge
        .restart_with(
            |config| {
                config.sign_in_sites.clear();
                for tenant in &mut config.tenants {
                    for connection in &mut tenant.connections {
                        connection.lookup_sites.clear();
                    }
                }
            },
            with_secret(),
        )
        .await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    assert!(
        capabilities.get("sign_in_sites").is_none(),
        "{capabilities}"
    );
    // No new sign-in, but Player 1 still sees their account and disconnects it,
    // which ends the link it approved.
    let (status, body) = STARTGG.try_start(&bridge, &player1).await;
    assert_eq!((status, code(&body)), (StatusCode::NOT_FOUND, "not_found"));
    assert_eq!(
        STARTGG.account(&bridge, &player1).await.as_deref(),
        Some(PLAYER1.0)
    );
    assert!(link_of(&bridge, GG, &player1).await.is_some());
    // Without a session, a removed site with accounts answers as one never
    // configured does, so nobody learns which removed sites have accounts.
    for path in ["/v1/sites/startgg", "/v1/sites/nope"] {
        let (status, body) = bridge.get("", path).await;
        assert_eq!(
            (status, code(&body)),
            (StatusCode::UNAUTHORIZED, "unauthenticated"),
            "{path}"
        );
        let response = bridge.client.delete(bridge.url(path)).send().await.unwrap();
        assert_eq!(response.status(), StatusCode::UNAUTHORIZED, "{path}");
    }
    let (status, body) = STARTGG.disconnect(&bridge, &player1).await;
    assert_eq!(status, StatusCode::OK, "{body}");
    assert_eq!(link_of(&bridge, GG, &player1).await, None);
    // With no account left on it, the site is unknown.
    let (status, body) = bridge.get(player1.token(), "/v1/sites/startgg").await;
    assert_eq!((status, code(&body)), (StatusCode::NOT_FOUND, "not_found"));
}

#[tokio::test]
async fn an_organizer_sign_in_asks_start_gg_for_the_reporter_scope() {
    let fake = fake_startgg();
    let bridge = bridge(&fake).await;
    let player1 = player(&bridge, 1).await;
    let url = STARTGG.start_organizing(&bridge, &player1).await;
    let scope = "user.identity tournament.reporter";
    assert_eq!(sign_in::query(&url)["scope"], scope);
    let page = STARTGG.organize(&bridge, &player1, "good-player1").await;
    assert!(page.contains("start.gg connected"), "{page}");
    // start.gg's token request names the scope too: the organizer one.
    assert_eq!(fake.seen("/oauth/access_token")[0].form["scope"], scope);
    assert_eq!(
        STARTGG.organizer(&bridge, &player1).await["scope"],
        json!(scope)
    );
}
