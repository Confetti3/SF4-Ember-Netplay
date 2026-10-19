//! Challonge sign-in against a stand-in Challonge. It runs on the same flow
//! as Discord's (tests/discord.rs covers that flow in depth), so these cover
//! what is Challonge's own: the authorize page, a form-only token exchange
//! with the client secret, `/me` with Challonge's headers, the pages naming
//! Challonge, the monthly request budget, answers racing each other, and
//! connections that find players by Challonge username. No token leaves the
//! bridge.
mod common;
#[path = "challonge/support.rs"]
mod support;

use common::{Bridge, code, sign_in};
use ember_bridge::{config::sites::Limits, integrations::Secrets};
use ember_protocol::challenge::{Action, Method};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use support::*;

#[tokio::test]
async fn a_player_connects_challonge_through_the_bridge_page() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    assert!(
        capabilities["features"]
            .as_array()
            .unwrap()
            .contains(&json!("sites"))
    );
    let origin = url::Url::parse(&bridge.state().config.sign_in_sites[0].authorize_url).unwrap();
    assert_eq!(
        capabilities["sign_in_sites"],
        json!([{
            "id": "challonge", "display_name": "Challonge", "sign_in": true, "lookup_by": "username",
            "authorize_origin": origin.origin().ascii_serialization(),
            "authorize_path": "/oauth/authorize",
        }])
    );
    let player1 = player(&bridge, 1).await;
    assert_eq!(CHALLONGE.account(&bridge, &player1).await, None);

    let (status, started) = CHALLONGE.try_start(&bridge, &player1).await;
    assert_eq!(status, StatusCode::CREATED, "{started}");
    assert_eq!(
        started["expires_at"].as_u64(),
        Some(bridge.clock.now() + 600)
    );
    let url = url::Url::parse(started["authorize_url"].as_str().unwrap()).unwrap();
    assert_eq!(url.path(), "/oauth/authorize");
    let query: std::collections::HashMap<_, _> = url.query_pairs().into_owned().collect();
    assert_eq!(query["response_type"], "code");
    assert_eq!(query["client_id"], CLIENT_ID);
    assert_eq!(query["scope"], "me");
    assert_eq!(
        query["redirect_uri"],
        bridge.url("/v1/sites/challonge/callback")
    );
    assert!(!query.contains_key("prompt"));
    // Starting asks Challonge nothing.
    assert!(fake.seen("/oauth/token").is_empty());

    // The page names the Ember ID and connects nothing until the player says so.
    let page = CHALLONGE
        .back(
            &bridge,
            &format!("code=good-player1&state={}", query["state"]),
        )
        .await;
    assert!(
        page.contains("Connect this Challonge account?")
            && page.contains(PLAYER1.1)
            && page.contains(&player1.id().fingerprint())
            && page.contains("Cups")
            && page.contains("action=\"/v1/sites/challonge/callback\""),
        "{page}"
    );
    assert_eq!(CHALLONGE.account(&bridge, &player1).await, None);
    let page = CHALLONGE
        .decide(&bridge, &confirmation(&page), "connect")
        .await;
    assert!(
        page.contains("Challonge connected") && page.contains(PLAYER1.1),
        "{page}"
    );
    let (_, connected) = bridge.get(player1.token(), "/v1/sites/challonge").await;
    assert_eq!(connected["account"]["user_id"], PLAYER1.0);
    assert_eq!(connected["account"]["username"], PLAYER1.1);

    // The code is exchanged in a form body only, with the client secret.
    let exchange = &fake.seen("/oauth/token")[0];
    assert!(exchange.query.is_empty(), "{:?}", exchange.query);
    assert_eq!(exchange.form["grant_type"], "authorization_code");
    assert_eq!(exchange.form["code"], "good-player1");
    assert_eq!(exchange.form["client_id"], CLIENT_ID);
    assert_eq!(exchange.form["client_secret"], CLIENT_SECRET);
    assert_eq!(
        exchange.form["redirect_uri"],
        bridge.url("/v1/sites/challonge/callback")
    );
    let me = &fake.seen("/v2.1/me.json")[0];
    assert_eq!(me.headers["authorization-type"], "v2");
    assert_eq!(me.headers["accept"], "application/json");
    // A GET with the definition's Content-Type, once.
    assert_eq!(content_types(&me.headers), ["application/vnd.api+json"]);
    assert_eq!(
        me.headers["authorization"],
        format!("Bearer {}", fake.tokens()[0])
    );

    // The token went nowhere but Challonge.
    assert!(!page.contains("access-token") && !connected.to_string().contains("access-token"));
    assert!(!database_holds(&bridge, "access-token").await);
    assert!(!database_holds(&bridge, "refresh-token").await);
}

#[tokio::test]
async fn an_account_on_another_ember_id_moves_only_when_the_player_says_so() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let (one, two) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &one, "player1").await;
    assert_eq!(
        CHALLONGE
            .found(&bridge, BRACKETS, &["player_1"])
            .await
            .len(),
        1
    );

    // The page names both Ember IDs; keeping it changes nothing.
    let page = CHALLONGE.sign_in(&bridge, &two, "good-player1").await;
    assert!(
        page.contains("Move this Challonge account?")
            && page.contains(&one.id().fingerprint())
            && page.contains(&two.id().fingerprint()),
        "{page}"
    );
    let kept = CHALLONGE
        .decide(&bridge, &confirmation(&page), "keep")
        .await;
    assert!(kept.contains("Nothing moved"), "{kept}");
    assert_eq!(
        CHALLONGE.account(&bridge, &one).await.as_deref(),
        Some(PLAYER1.0)
    );
    assert_eq!(CHALLONGE.account(&bridge, &two).await, None);

    // Moved: the link it approved for the first Ember ID ends, and the next
    // lookup links it where it now is.
    let page = CHALLONGE.sign_in(&bridge, &two, "good-player1").await;
    let moved = CHALLONGE
        .decide(&bridge, &confirmation(&page), "move")
        .await;
    assert!(moved.contains("Challonge connected"), "{moved}");
    assert_eq!(CHALLONGE.account(&bridge, &one).await, None);
    assert_eq!(
        CHALLONGE.account(&bridge, &two).await.as_deref(),
        Some(PLAYER1.0)
    );
    assert_eq!(link_of(&bridge, BRACKETS, &one).await, None);
    assert_eq!(
        CHALLONGE.found(&bridge, BRACKETS, &["PLAYER_1"]).await[0].2,
        two.id().as_str()
    );

    // A move page left open while the account moved again changes nothing.
    let page = CHALLONGE.sign_in(&bridge, &one, "good-player1").await;
    CHALLONGE.disconnect(&bridge, &two).await;
    let changed = CHALLONGE
        .decide(&bridge, &confirmation(&page), "move")
        .await;
    assert!(changed.contains("after this page asked"), "{changed}");
    assert_eq!(CHALLONGE.account(&bridge, &one).await, None);
}

#[tokio::test]
async fn the_latest_sign_in_replaces_an_ember_ids_account() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let player1 = player(&bridge, 1).await;
    connect(&bridge, &player1, "player1").await;
    CHALLONGE.found(&bridge, BRACKETS, &[PLAYER1.1]).await;
    assert_eq!(
        link_of(&bridge, BRACKETS, &player1).await,
        Some(("site".into(), format!("challonge:{}", PLAYER1.0)))
    );
    // Another account for the same Ember ID: the page says what it replaces,
    // and connecting it ends the first one's links.
    let page = CHALLONGE.sign_in(&bridge, &player1, "good-player2").await;
    assert!(
        page.contains("is connected to Challonge account <strong>Player_1</strong> now"),
        "{page}"
    );
    CHALLONGE
        .decide(&bridge, &confirmation(&page), "connect")
        .await;
    assert_eq!(
        CHALLONGE.account(&bridge, &player1).await.as_deref(),
        Some(PLAYER2.0)
    );
    assert_eq!(link_of(&bridge, BRACKETS, &player1).await, None);
    assert_eq!(CHALLONGE.found(&bridge, BRACKETS, &[PLAYER1.1]).await, []);
    assert_eq!(
        CHALLONGE.found(&bridge, BRACKETS, &[PLAYER2.1]).await.len(),
        1
    );
}

#[tokio::test]
async fn challonge_refusing_or_failing_connects_nothing() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let player1 = player(&bridge, 1).await;
    for (code, says) in [
        ("busy", "Challonge did not answer"),
        (
            "odd-name",
            "username <strong>has space</strong> cannot be used to find you",
        ),
        ("made-up", "Challonge did not answer"),
        // Lookups match usernames, so an account without one is no answer.
        ("no-name", "Challonge did not answer"),
    ] {
        let page = CHALLONGE.sign_in(&bridge, &player1, code).await;
        assert!(page.contains(says), "{code}: {page}");
    }
    let state = CHALLONGE.start(&bridge, &player1).await;
    let page = CHALLONGE
        .back(&bridge, &format!("error=access_denied&state={state}"))
        .await;
    assert!(
        page.contains("Challonge did not connect your account"),
        "{page}"
    );
    // Each sign-in is over: its state answers nothing again.
    let page = CHALLONGE
        .back(&bridge, &format!("code=good-player1&state={state}"))
        .await;
    assert!(page.contains("Sign-in expired"), "{page}");
    assert_eq!(CHALLONGE.account(&bridge, &player1).await, None);
    // A 429 is one of the exchanges Challonge saw.
    assert_eq!(fake.seen("/oauth/token").len(), 4);
}

#[tokio::test]
async fn a_second_answer_or_a_cancel_while_challonge_checks_wins_nothing() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let player1 = player(&bridge, 1).await;

    // Reloaded while Challonge still checks the first answer.
    let state = CHALLONGE.start(&bridge, &player1).await;
    let url = bridge.url(&format!(
        "/v1/sites/challonge/callback?code=held-player1&state={state}"
    ));
    let client = bridge.client.clone();
    let first =
        tokio::spawn(async move { client.get(url).send().await.unwrap().text().await.unwrap() });
    HELD.arrived.notified().await;
    let page = CHALLONGE
        .back(&bridge, &format!("code=held-player1&state={state}"))
        .await;
    assert!(page.contains("Sign-in expired"), "{page}");
    HELD.release.notify_one();
    let page = first.await.unwrap();
    assert!(page.contains("Connect this Challonge account?"), "{page}");

    // Cancelled in Ember while Challonge checks: the answer connects nothing.
    let state = CHALLONGE.start(&bridge, &player1).await;
    let url = bridge.url(&format!(
        "/v1/sites/challonge/callback?code=held-player1&state={state}"
    ));
    let client = bridge.client.clone();
    let late =
        tokio::spawn(async move { client.get(url).send().await.unwrap().text().await.unwrap() });
    HELD.arrived.notified().await;
    assert_eq!(CHALLONGE.cancel(&bridge, &player1).await.0, StatusCode::OK);
    HELD.release.notify_one();
    let page = late.await.unwrap();
    assert!(page.contains("Sign-in expired"), "{page}");
    assert_eq!(CHALLONGE.account(&bridge, &player1).await, None);

    // Disconnected in Ember while Challonge checks: the same.
    connect(&bridge, &player1, "player2").await;
    let state = CHALLONGE.start(&bridge, &player1).await;
    let url = bridge.url(&format!(
        "/v1/sites/challonge/callback?code=held-player1&state={state}"
    ));
    let client = bridge.client.clone();
    let late =
        tokio::spawn(async move { client.get(url).send().await.unwrap().text().await.unwrap() });
    HELD.arrived.notified().await;
    assert_eq!(
        CHALLONGE.disconnect(&bridge, &player1).await.0,
        StatusCode::OK
    );
    HELD.release.notify_one();
    let page = late.await.unwrap();
    assert!(page.contains("Sign-in expired"), "{page}");
    assert_eq!(CHALLONGE.account(&bridge, &player1).await, None);
}

#[tokio::test]
async fn sign_in_is_off_without_the_client_secret_but_an_account_can_go() {
    let fake = fake_challonge();
    let bridge = bridge_with(&fake, LIMITS, Secrets::default()).await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    assert_eq!(capabilities["sign_in_sites"][0]["sign_in"], false);
    let player1 = player(&bridge, 1).await;
    let ember_id = player1.id().as_str().to_owned();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO site_accounts (site, user_id, ember_id, username, username_key, connected_at)
                 VALUES ('challonge', '1001', ?1, 'Player_1', 'player_1', 1)",
                [&ember_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();

    assert_eq!(
        CHALLONGE.try_start(&bridge, &player1).await.0,
        StatusCode::NOT_FOUND
    );
    assert!(
        CHALLONGE
            .back(&bridge, "code=good-player1&state=x")
            .await
            .contains("Challonge sign-in is off")
    );
    assert_eq!(CHALLONGE.cancel(&bridge, &player1).await.0, StatusCode::OK);
    assert_eq!(
        CHALLONGE.account(&bridge, &player1).await.as_deref(),
        Some("1001")
    );
    assert_eq!(
        CHALLONGE.disconnect(&bridge, &player1).await,
        (StatusCode::OK, json!({ "account": null }))
    );
    assert_eq!(CHALLONGE.account(&bridge, &player1).await, None);
    assert!(fake.seen("/oauth/token").is_empty());
}

#[tokio::test]
async fn a_start_needs_its_own_proof() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let player1 = player(&bridge, 1).await;
    let body = bridge
        .prove(
            &player1,
            Action::SiteCancel,
            Method::Delete,
            "/v1/sites/challonge/start",
            json!({}),
        )
        .await;
    let (status, body) = bridge
        .send_proof(&player1, Method::Post, "/v1/sites/challonge/start", body)
        .await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::UNAUTHORIZED, "invalid_signature"),
        "{body}"
    );
    // There is no move action: the page asks the browser's user instead.
    let (status, _) = bridge
        .post(
            player1.token(),
            "/v1/auth/challenges",
            json!({
                "public_key": player1.identity.public_key(),
                "action": "challonge.move",
                "method": "POST",
                "path": "/v1/sites/challonge/move",
                "request_digest": "0".repeat(43),
            }),
        )
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
}

#[tokio::test]
async fn the_bridge_keeps_to_its_monthly_challonge_budget() {
    let fake = fake_challonge();
    let bridge = bridge_with(
        &fake,
        Limits {
            monthly_request_budget: 4,
            ..LIMITS
        },
        with_secret(),
    )
    .await;
    // The first second of a month, UTC (2026-11-01).
    let month_start: i64 = 1_793_491_200;
    bridge
        .clock
        .advance(month_start - bridge.clock.now() as i64);
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);

    // Two sign-ins started while there is room for both exchanges...
    let player1_state = CHALLONGE.start(&bridge, &player1).await;
    let player2_state = CHALLONGE.start(&bridge, &player2).await;
    connect_with(&bridge, &player1_state, "player1").await;
    assert_eq!(CHALLONGE.requests_spent(&bridge).await, 2);
    // ...but the second exchange spends the last of the budget.
    connect_with(&bridge, &player2_state, "player2").await;
    assert_eq!(CHALLONGE.requests_spent(&bridge).await, 4);
    assert_eq!(fake.seen("/oauth/token").len(), 2);
    // Now starts are refused as busy, which Ember tells apart from the
    // service failing.
    let (status, body) = CHALLONGE.try_start(&bridge, &player1).await;
    assert_eq!(
        (status, code(&body), &body["error"]["retryable"]),
        (
            StatusCode::SERVICE_UNAVAILABLE,
            "sign_in_busy",
            &json!(true)
        ),
        "{body}"
    );

    // The next month starts afresh.
    bridge.clock.advance(30 * 24 * 3600);
    let mut player1 = player1;
    bridge.open_session(&mut player1).await;
    // Signing in again with the account it has connects without asking.
    connect(&bridge, &player1, "player1").await;
    assert_eq!(CHALLONGE.requests_spent(&bridge).await, 2);
}

#[tokio::test]
async fn throwaway_ember_ids_spend_at_most_a_days_share() {
    let fake = fake_challonge();
    let bridge = bridge_with(
        &fake,
        Limits {
            daily_request_budget: 4,
            monthly_request_budget: 10,
            ..LIMITS
        },
        with_secret(),
    )
    .await;
    // Each new Ember ID starts a sign-in and answers it with a made-up code,
    // which Challonge refuses after the bridge has spent the exchange.
    let mut players = Vec::new();
    for byte in 1..=3 {
        players.push(player(&bridge, byte).await);
    }
    for player in &players[..2] {
        let page = CHALLONGE.sign_in(&bridge, player, "made-up").await;
        assert!(page.contains("Challonge did not answer"), "{page}");
    }
    assert_eq!(CHALLONGE.requests_spent(&bridge).await, 4);
    // The day's share is spent: a start is refused as busy.
    let (status, body) = CHALLONGE.try_start(&bridge, &players[2]).await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::SERVICE_UNAVAILABLE, "sign_in_busy"),
        "{body}"
    );
    // The month keeps the rest for the next days.
    bridge.clock.advance(24 * 3600);
    let mut player1 = bridge.player(9);
    bridge.open_session(&mut player1).await;
    connect(&bridge, &player1, "player1").await;
    assert_eq!(CHALLONGE.requests_spent(&bridge).await, 6);
    assert_eq!(fake.seen("/oauth/token").len(), 3);
}

#[tokio::test]
async fn starts_are_limited_across_the_bridge_too() {
    let fake = fake_challonge();
    let bridge = bridge_with(
        &fake,
        Limits {
            starts_per_hour: 2,
            ..LIMITS
        },
        with_secret(),
    )
    .await;
    let players = [
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
    ];
    CHALLONGE.start(&bridge, &players[0]).await;
    CHALLONGE.start(&bridge, &players[1]).await;
    let (status, body) = CHALLONGE.try_start(&bridge, &players[2]).await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::SERVICE_UNAVAILABLE, "sign_in_busy"),
        "{body}"
    );
    // An hour later there is room again.
    bridge.clock.advance(3600);
    let mut third = bridge.player(3);
    bridge.open_session(&mut third).await;
    CHALLONGE.start(&bridge, &third).await;
}

#[tokio::test]
async fn a_sign_in_started_with_room_is_refused_once_the_budget_is_spent() {
    let fake = fake_challonge();
    let bridge = bridge_with(
        &fake,
        Limits {
            monthly_request_budget: 2,
            ..LIMITS
        },
        with_secret(),
    )
    .await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let player1_state = CHALLONGE.start(&bridge, &player1).await;
    let player2_state = CHALLONGE.start(&bridge, &player2).await;
    connect_with(&bridge, &player2_state, "player2").await;
    let page = CHALLONGE
        .back(&bridge, &format!("code=good-player1&state={player1_state}"))
        .await;
    assert!(page.contains("Ember cannot check with Challonge"), "{page}");
    assert_eq!(fake.seen("/oauth/token").len(), 1);
    assert_eq!(CHALLONGE.account(&bridge, &player1).await, None);
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
async fn a_lookup_finds_connected_players_by_username_without_case() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &player1, "player1").await;
    connect(&bridge, &player2, "player2").await;
    let provider = bridge.provider(BRACKETS).await;
    let after = bridge.events(&provider, "0").await.len();

    // In the order asked, as asked, once each; unknown names are left out.
    let answer = CHALLONGE
        .found(
            &bridge,
            BRACKETS,
            &["PLAYER2", "nobody", "player_1", "Player_1"],
        )
        .await;
    assert_eq!(
        answer,
        [
            (
                "PLAYER2".to_owned(),
                PLAYER2.0.to_owned(),
                player2.id().to_string()
            ),
            (
                "player_1".to_owned(),
                PLAYER1.0.to_owned(),
                player1.id().to_string()
            ),
        ]
    );
    for (player, user) in [(&player1, PLAYER1), (&player2, PLAYER2)] {
        assert_eq!(
            link_of(&bridge, BRACKETS, player).await,
            Some(("site".into(), format!("challonge:{}", user.0)))
        );
    }
    let events = bridge.events(&provider, "0").await;
    let completed: Vec<_> = events[after..]
        .iter()
        .filter(|event| event["type"] == "io.ember.tournament.identity.link.completed.v1")
        .map(|event| event["data"]["approved_via"].clone())
        .collect();
    assert_eq!(completed, [json!("site"), json!("site")]);
    let (_, first) = lookup(&bridge, BRACKETS, &["player2"], &[]).await;
    let (_, again) = lookup(&bridge, BRACKETS, &["player2"], &[]).await;
    assert_eq!(first, again);
    assert_eq!(first["players"], json!([]));

    // A name that is not a Challonge username finds no one, and the rest
    // of the lookup still answers; too many names are refused.
    assert_eq!(
        CHALLONGE
            .found(
                &bridge,
                BRACKETS,
                &["has space", "", "player1@example.com", "PLAYER2"]
            )
            .await,
        [(
            "PLAYER2".to_owned(),
            PLAYER2.0.to_owned(),
            player2.id().to_string()
        )]
    );
    let names: Vec<String> = (0..20).map(|n| format!("player{n}")).collect();
    let names: Vec<&str> = names.iter().map(String::as_str).collect();
    let ids: Vec<String> = (0..13)
        .map(|n| (10_000_000_000_000_000u64 + n).to_string())
        .collect();
    let ids: Vec<&str> = ids.iter().map(String::as_str).collect();
    let (status, _) = lookup(&bridge, BOTH, &names, &ids).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = lookup(&bridge, BOTH, &names, &ids[..12]).await;
    assert_eq!(status, StatusCode::OK);
}

#[tokio::test]
async fn a_name_another_account_took_answers_for_that_account_only() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &player1, "player1").await;
    let connected_at = bridge.clock.now();
    let (_, answer) = lookup(&bridge, BRACKETS, &[PLAYER1.1], &[]).await;
    assert_eq!(answer["accounts"][0]["connected_at"], connected_at);

    // Player 1 renamed on Challonge, and another account took the old name.
    bridge.clock.advance(60);
    connect(&bridge, &player2, "player3").await;
    let taken = CHALLONGE.found(&bridge, BOTH, &["player_1"]).await;
    assert_eq!(
        taken,
        [(
            "player_1".to_owned(),
            PLAYER3.0.to_owned(),
            player2.id().to_string()
        )]
    );
    // Player 1's account stays connected; it answers again for the name it
    // next connects with.
    assert_eq!(
        CHALLONGE.account(&bridge, &player1).await.as_deref(),
        Some(PLAYER1.0)
    );
    bridge.clock.advance(60);
    connect(&bridge, &player1, "player1").await;
    assert_eq!(
        CHALLONGE.found(&bridge, BOTH, &["Player_1"]).await[0].2,
        player1.id().as_str()
    );
}

#[tokio::test]
async fn a_lookup_that_names_no_list_finds_no_one() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    for connection in [BRACKETS, DISCORD_ONLY, BOTH] {
        let provider = bridge.provider(connection).await;
        let (status, body) = bridge
            .post(&provider, "/v1/players/lookup", json!({}))
            .await;
        assert_eq!(
            (status, body),
            (StatusCode::OK, json!({ "players": [] })),
            "{connection}"
        );
    }
}

#[tokio::test]
async fn each_list_needs_its_own_setting() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let player1 = player(&bridge, 1).await;
    connect(&bridge, &player1, "player1").await;
    for (connection, challonge, discord) in [
        (DISCORD_ONLY, vec![PLAYER1.1], vec![]),
        (BRACKETS, vec![PLAYER1.1], vec!["274220342558756145"]),
        ("mock-a", vec![PLAYER1.1], vec![]),
        ("mock-a", vec![], vec![]),
    ] {
        let (status, body) = lookup(&bridge, connection, &challonge, &discord).await;
        assert_eq!(
            (status, code(&body)),
            (StatusCode::FORBIDDEN, "forbidden"),
            "{connection}"
        );
    }
    // Asking nothing of a list is not asking for it.
    let (status, body) = lookup(&bridge, DISCORD_ONLY, &[], &["274220342558756145"]).await;
    assert_eq!((status, body), (StatusCode::OK, json!({ "players": [] })));
    assert_eq!(
        CHALLONGE.found(&bridge, BRACKETS, &[PLAYER1.1]).await.len(),
        1
    );
}

#[tokio::test]
async fn discord_and_challonge_find_the_same_player_as_one_link() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &player1, "player1").await;
    connect(&bridge, &player2, "player2").await;
    let discord_player1 = "274220342558756145";
    connect_discord(&bridge, discord_player1, &player1).await;
    let (status, answer) = lookup(&bridge, BOTH, &[PLAYER1.1, PLAYER2.1], &[discord_player1]).await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    // Player 1 is linked once, by Discord, which was asked first; both answers
    // name the same participant.
    let discord_found = &answer["players"][0];
    let challonge_found = answer["accounts"].as_array().unwrap();
    assert_eq!(discord_found["ember_id"], player1.id().as_str());
    assert_eq!(challonge_found.len(), 2);
    assert_eq!(challonge_found[0]["ember_id"], player1.id().as_str());
    assert_eq!(
        challonge_found[0]["participant_id"],
        discord_found["participant_id"]
    );
    assert_eq!(
        link_of(&bridge, BOTH, &player1).await,
        Some(("discord".into(), format!("discord:{discord_player1}")))
    );
    assert_eq!(link_of(&bridge, BOTH, &player2).await.unwrap().0, "site");
}

#[tokio::test]
async fn a_discord_lookup_after_challonge_uses_challonges_link_until_it_ends() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let player1 = player(&bridge, 1).await;
    connect(&bridge, &player1, "player1").await;
    let discord_player1 = "274220342558756145";
    connect_discord(&bridge, discord_player1, &player1).await;

    // Challonge links Player 1 first; Discord then finds the same participant.
    let challonge = CHALLONGE.found(&bridge, BOTH, &[PLAYER1.1]).await;
    assert_eq!(challonge.len(), 1);
    let (_, by_discord) = lookup(&bridge, BOTH, &[], &[discord_player1]).await;
    let (_, by_challonge) = lookup(&bridge, BOTH, &[PLAYER1.1], &[]).await;
    assert_eq!(
        by_discord["players"][0]["participant_id"],
        by_challonge["accounts"][0]["participant_id"]
    );
    assert_eq!(link_of(&bridge, BOTH, &player1).await.unwrap().0, "site");

    // Disconnecting Challonge ends that link; Discord links Player 1 again itself.
    CHALLONGE.disconnect(&bridge, &player1).await;
    assert_eq!(link_of(&bridge, BOTH, &player1).await, None);
    let (_, again) = lookup(&bridge, BOTH, &[], &[discord_player1]).await;
    assert_eq!(again["players"][0]["ember_id"], player1.id().as_str());
    assert_eq!(
        link_of(&bridge, BOTH, &player1).await,
        Some(("discord".into(), format!("discord:{discord_player1}")))
    );
}

#[tokio::test]
async fn a_link_the_player_removed_is_not_made_again_until_a_new_sign_in() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &player1, "player1").await;
    connect(&bridge, &player2, "player2").await;
    assert_eq!(
        CHALLONGE
            .found(&bridge, BRACKETS, &[PLAYER1.1, PLAYER2.1])
            .await
            .len(),
        2
    );

    remove_link(&bridge, &player1, BRACKETS).await;
    for _ in 0..2 {
        let names: Vec<_> = CHALLONGE
            .found(&bridge, BRACKETS, &[PLAYER1.1, PLAYER2.1])
            .await
            .into_iter()
            .map(|(name, ..)| name)
            .collect();
        assert_eq!(names, [PLAYER2.1]);
    }
    // Other connections still find Player 1.
    assert_eq!(CHALLONGE.found(&bridge, BOTH, &[PLAYER1.1]).await.len(), 1);
    // A sign-in afterwards gives the consent back.
    connect(&bridge, &player1, "player1").await;
    assert_eq!(
        CHALLONGE.found(&bridge, BRACKETS, &[PLAYER1.1]).await.len(),
        1
    );
}

#[tokio::test]
async fn one_account_at_most_answers_for_a_username() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let players = [
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
    ];
    insert_account(&bridge, SITE, "2001", &players[0], Some("renamed"))
        .await
        .unwrap();
    // A second account under the same name is refused by the schema, while
    // accounts whose name another took (NULL) are as many as there are.
    assert!(
        insert_account(&bridge, SITE, "2002", &players[1], Some("renamed"))
            .await
            .is_err()
    );
    insert_account(&bridge, SITE, "2002", &players[1], None)
        .await
        .unwrap();
    insert_account(&bridge, SITE, "2003", &players[2], None)
        .await
        .unwrap();
    assert_eq!(
        CHALLONGE.found(&bridge, BRACKETS, &["RENAMED"]).await,
        [(
            "RENAMED".to_owned(),
            "2001".to_owned(),
            players[0].id().to_string()
        )]
    );
}

#[tokio::test]
async fn starts_with_bad_proofs_take_no_bridge_wide_slot() {
    let fake = fake_challonge();
    let bridge = bridge_with(
        &fake,
        Limits {
            starts_per_hour: 2,
            ..LIMITS
        },
        with_secret(),
    )
    .await;
    let players = [player(&bridge, 1).await, player(&bridge, 2).await];
    // Proofs made for another action, sent as starts, many times over.
    for _ in 0..3 {
        let body = bridge
            .prove(
                &players[0],
                Action::SiteCancel,
                Method::Delete,
                "/v1/sites/challonge/start",
                json!({}),
            )
            .await;
        let (status, _) = bridge
            .send_proof(&players[0], Method::Post, "/v1/sites/challonge/start", body)
            .await;
        assert_eq!(status, StatusCode::UNAUTHORIZED);
    }
    // Both slots are still there for proven starts.
    CHALLONGE.start(&bridge, &players[0]).await;
    CHALLONGE.start(&bridge, &players[1]).await;
    let (status, body) = CHALLONGE.try_start(&bridge, &players[1]).await;
    assert_eq!(code(&body), "sign_in_busy", "{status}");
}

#[tokio::test]
async fn a_start_that_does_not_commit_gives_its_bridge_wide_slot_back() {
    let fake = fake_challonge();
    let bridge = bridge_with(
        &fake,
        Limits {
            starts_per_hour: 1,
            ..LIMITS
        },
        with_secret(),
    )
    .await;
    let player1 = player(&bridge, 1).await;
    // The sign-ins table is gone for a moment, so the start's write fails
    // after it took the hour's only slot.
    let sign_ins = |sql: &'static str| {
        bridge.state().db.write(move |tx| {
            tx.execute_batch(sql)?;
            Ok(())
        })
    };
    sign_ins("ALTER TABLE site_sign_ins RENAME TO site_sign_ins_away")
        .await
        .unwrap();
    let (status, body) = CHALLONGE.try_start(&bridge, &player1).await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::SERVICE_UNAVAILABLE, "service_unavailable"),
        "{body}"
    );
    sign_ins("ALTER TABLE site_sign_ins_away RENAME TO site_sign_ins")
        .await
        .unwrap();
    // The slot was given back: the next start has it.
    CHALLONGE.start(&bridge, &player1).await;
    let (_, body) = CHALLONGE.try_start(&bridge, &player1).await;
    assert_eq!(code(&body), "sign_in_busy", "{body}");
}

#[tokio::test]
async fn a_bridge_without_sign_in_sites_lists_none() {
    let bridge = Bridge::start().await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    let features = capabilities["features"].as_array().unwrap();
    assert!(features.contains(&json!("discord.accounts")));
    assert!(
        capabilities.get("sign_in_sites").is_none(),
        "{capabilities}"
    );
    let player1 = player(&bridge, 1).await;
    let (status, _) = CHALLONGE.try_start(&bridge, &player1).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
}

#[tokio::test]
async fn an_organizer_sign_in_keeps_its_tokens_sealed_and_refreshes_them() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let mut player1 = player(&bridge, 1).await;
    assert_eq!(CHALLONGE.organizer(&bridge, &player1).await, json!(null));

    // It asks Challonge for the organizer scopes, and connects the account
    // as a player sign-in does.
    let url = CHALLONGE.start_organizing(&bridge, &player1).await;
    let scope = "me tournaments:read tournaments:write matches:read matches:write participants:read \
        participants:write";
    assert_eq!(query(&url)["scope"], scope);
    let page = CHALLONGE.organize(&bridge, &player1, "good-player1").await;
    assert!(page.contains("Challonge connected"), "{page}");
    assert_eq!(
        CHALLONGE.account(&bridge, &player1).await.as_deref(),
        Some(PLAYER1.0)
    );
    let now = bridge.clock.now();
    assert_eq!(
        CHALLONGE.organizer(&bridge, &player1).await,
        json!({ "scope": scope, "expires_at": now + 604800 })
    );
    // Kept, but never in the clear.
    assert!(!database_holds(&bridge, "access-token").await);
    assert!(!database_holds(&bridge, "refresh-token").await);

    // A day before it lapses, reading it refreshes it.
    bridge.clock.advance(604800 - 86400 + 1);
    bridge.open_session(&mut player1).await;
    let expires = bridge.clock.now() + 604800;
    assert_eq!(
        CHALLONGE.organizer(&bridge, &player1).await,
        json!({ "scope": scope, "expires_at": expires })
    );
    let refresh = fake.seen("/oauth/token").pop().unwrap();
    assert_eq!(refresh.form["grant_type"], "refresh_token");
    assert!(refresh.form["refresh_token"].starts_with("refresh-token-1001"));
    assert_eq!(refresh.form["client_secret"], CLIENT_SECRET);
    assert!(!database_holds(&bridge, "refresh-token").await);

    // Signing in again as a player with the same account keeps it.
    CHALLONGE.connect(&bridge, &player1, "good-player1").await;
    assert_eq!(
        CHALLONGE.organizer(&bridge, &player1).await,
        json!({ "scope": scope, "expires_at": expires })
    );

    // Withdrawing keeps the account; disconnecting it takes the grant along.
    let (status, _) = CHALLONGE.withdraw(&bridge, &player1).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(CHALLONGE.organizer(&bridge, &player1).await, json!(null));
    assert!(CHALLONGE.account(&bridge, &player1).await.is_some());
    CHALLONGE.organize(&bridge, &player1, "good-player1").await;
    assert!(CHALLONGE.organizer(&bridge, &player1).await.is_object());
    let (status, _) = CHALLONGE.disconnect(&bridge, &player1).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(CHALLONGE.organizer(&bridge, &player1).await, json!(null));
}

// An organizer lists the Challonge events they run, has Ember run one, and
// the desk connection's bracket connector gets it with a fresh token, for as
// long as the organizer runs it and keeps their organizer access.
#[tokio::test]
async fn an_organizer_runs_their_own_event_on_the_desk() {
    let fake = fake_challonge();
    fake.lists("/v2.1/tournaments.json", |(id, _)| {
        if id == PLAYER1.0 {
            json!({ "data": [
                { "id": "9001", "type": "tournament", "attributes": { "name": "Ember Weekly 12" } },
                { "id": 9002, "type": "tournament", "attributes": { "name": "Ember Weekly 13" } },
            ] })
        } else {
            json!({ "data": [] })
        }
    });
    let bridge = desk_bridge(&fake).await;
    let player1 = player(&bridge, 1).await;
    let player2 = player(&bridge, 2).await;

    // Without organizer access there is nothing to list.
    let (status, _) = CHALLONGE.tournaments(&bridge, &player1).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    CHALLONGE.organize(&bridge, &player1, "good-player1").await;
    let (status, listed) = CHALLONGE.tournaments(&bridge, &player1).await;
    assert_eq!(status, StatusCode::OK, "{listed}");
    assert_eq!(
        listed["tournaments"],
        json!([
            { "id": "9001", "name": "Ember Weekly 12" },
            { "id": "9002", "name": "Ember Weekly 13" },
        ])
    );
    let seen = fake.seen("/v2.1/tournaments.json").pop().unwrap();
    assert_eq!(seen.query["per_page"], "50");
    assert_eq!(seen.headers["authorization-type"], "v2");

    // Only an event the site lists for them, with settings the connector reads.
    let (status, _) = CHALLONGE
        .start_event(&bridge, &player1, "9999", json!({}))
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let credential = json!({ "credential": { "kind": "bearer" } });
    let (status, _) = CHALLONGE
        .start_event(&bridge, &player1, "9001", credential)
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let settings = json!({ "rounds": { "games_to_win": 2 }, "hold": { "streamed": true } });
    let (status, started) = CHALLONGE
        .start_event(&bridge, &player1, "9001", settings.clone())
        .await;
    assert_eq!(status, StatusCode::CREATED, "{started}");
    let id = started["id"].as_str().unwrap().to_owned();
    // An event runs once.
    let (status, _) = CHALLONGE
        .start_event(&bridge, &player1, "9001", json!({}))
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, events) = bridge.get(player1.token(), "/v1/organizer/events").await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(events["events"][0]["id"], id.as_str());
    assert_eq!(events["events"][0]["name"], "Ember Weekly 12");
    assert_eq!(events["events"][0]["settings"], settings);

    // The desk connection's provider gets it with the organizer's token; no
    // other connection does.
    let desk = bridge.provider(BRACKETS).await;
    let (status, running) = bridge.get(&desk, "/v1/desk/brackets").await;
    assert_eq!(status, StatusCode::OK, "{running}");
    let bracket = &running["brackets"][0];
    assert_eq!(
        (&bracket["site"], &bracket["tournament"]),
        (&json!(SITE), &json!("9001"))
    );
    assert!(
        bracket["access_token"]
            .as_str()
            .unwrap()
            .starts_with("access-token-1001")
    );
    let other = bridge.provider(BOTH).await;
    let (status, _) = bridge.get(&other, "/v1/desk/brackets").await;
    assert_eq!(status, StatusCode::FORBIDDEN);

    // Nobody else stops it.
    let (status, _) = stop_event(&bridge, &player2, &id).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    // Without organizer access it waits, and comes back with it.
    CHALLONGE.withdraw(&bridge, &player1).await;
    let (_, running) = bridge.get(&desk, "/v1/desk/brackets").await;
    assert_eq!(running["brackets"], json!([]));
    CHALLONGE.organize(&bridge, &player1, "good-player1").await;
    let (_, running) = bridge.get(&desk, "/v1/desk/brackets").await;
    assert_eq!(running["brackets"][0]["id"], id.as_str());

    // Stopped, it is gone.
    let (status, _) = stop_event(&bridge, &player1, &id).await;
    assert_eq!(status, StatusCode::OK);
    let (_, running) = bridge.get(&desk, "/v1/desk/brackets").await;
    assert_eq!(running["brackets"], json!([]));
    let (_, events) = bridge.get(player1.token(), "/v1/organizer/events").await;
    assert_eq!(events["events"], json!([]));
}

// An organizer makes a draft with only a title, has Ember create its event on
// Challonge with their grant, gives it links and a start time, and marks it
// ready: the desk's connector gets it from that time on.
#[tokio::test]
async fn an_organizer_drafts_an_event_and_runs_it_from_its_start_time() {
    let fake = fake_challonge();
    fake.lists("/v2.1/tournaments.json", |_| {
        json!({ "data": [{ "id": "9001", "type": "tournament", "attributes": { "name": "Weekly" } }] })
    });
    fake.creates(|_, sent| {
        json!({ "data": { "id": "777", "type": "tournament", "attributes": {
            "name": sent["data"]["attributes"]["name"], "url": "ember_cup" } } })
    });
    let bridge = desk_bridge(&fake).await;
    let player1 = player(&bridge, 1).await;
    let player2 = player(&bridge, 2).await;
    CHALLONGE.organize(&bridge, &player1, "good-player1").await;
    let organizer = |path: String, command| {
        let (bridge, player) = (&bridge, &player1);
        async move {
            sign_in::send(
                bridge,
                player,
                Action::SiteConnect,
                Method::Post,
                &path,
                command,
            )
            .await
        }
    };
    let events = || async { bridge.get(player1.token(), "/v1/organizer/events").await.1 };

    // A title is all a draft needs.
    let (status, _) = organizer(
        "/v1/organizer/drafts".into(),
        json!({ "title": "x".repeat(101) }),
    )
    .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, made) = organizer(
        "/v1/organizer/drafts".into(),
        json!({ "title": " Ember Cup " }),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED, "{made}");
    let id = made["id"].as_str().unwrap().to_owned();
    let path = |rest: &str| format!("/v1/organizer/events/{id}{rest}");
    let listed = events().await;
    assert_eq!(listed["events"][0]["title"], "Ember Cup");
    assert_eq!(listed["events"][0]["status"], "draft");

    // Not ready without its event and a start time.
    let (status, _) = organizer(path("/ready"), json!({})).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);

    // Ember creates it on Challonge, set as the site's fields allow.
    let create = |fields| json!({ "site": SITE, "fields": fields });
    let (status, _) = organizer(path("/create"), create(json!({ "format": "ladder" }))).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let fields = json!({ "format": "double elimination", "open_signup": true });
    let (status, created) = organizer(path("/create"), create(fields)).await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    assert_eq!(
        created,
        json!({ "tournament": "777", "link": "https://challonge.com/ember_cup" })
    );
    let sent: Json =
        serde_json::from_str(&fake.seen("/v2.1/tournaments.json").pop().unwrap().body).unwrap();
    let attributes = &sent["data"]["attributes"];
    assert_eq!(attributes["name"], "Ember Cup");
    assert_eq!(attributes["tournament_type"], "double elimination");
    assert_eq!(attributes["registration_options"]["open_signup"], true);
    // Once only.
    let (status, _) = organizer(path("/create"), create(json!({}))).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    // Links are https; the bracket's is there already.
    let listed = events().await;
    assert_eq!(listed["events"][0]["tournament"], "777");
    let bracket = listed["events"][0]["links"][0].clone();
    assert_eq!(bracket["label"], "Bracket");
    let stream = json!({ "label": "Stream", "url": "https://twitch.tv/ember" });
    let (status, _) = organizer(
        path(""),
        json!({ "links": [bracket, { "label": "Stream", "url": "http://twitch.tv/ember" }] }),
    )
    .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let starts_at = bridge.clock.now() + 300;
    let (status, changed) = organizer(
        path(""),
        json!({ "links": [bracket, stream], "starts_at": starts_at,
                "settings": { "rounds": { "games_to_win": 2 } } }),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{changed}");
    // Nobody else changes it.
    let (status, _) = sign_in::send(
        &bridge,
        &player2,
        Action::SiteConnect,
        Method::Post,
        &path(""),
        json!({ "title": "Mine" }),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    // Ready, it waits for its start time, then the connector gets it.
    let (status, _) = organizer(path("/ready"), json!({})).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(events().await["events"][0]["status"], "ready");
    let desk = bridge.provider(BRACKETS).await;
    let (_, running) = bridge.get(&desk, "/v1/desk/brackets").await;
    assert_eq!(running["brackets"], json!([]));
    bridge.clock.advance(300);
    let (_, running) = bridge.get(&desk, "/v1/desk/brackets").await;
    assert_eq!(running["brackets"][0]["tournament"], "777");
    let listed = events().await;
    assert_eq!(listed["events"][0]["status"], "running");
    assert_eq!(listed["events"][0]["links"][1], stream);

    // Another draft takes an event the site lists; deleted, it is gone.
    let (_, made) = organizer("/v1/organizer/drafts".into(), json!({ "title": "Weekly" })).await;
    let other = made["id"].as_str().unwrap().to_owned();
    let other_path = format!("/v1/organizer/events/{other}");
    let (status, _) = organizer(
        other_path.clone(),
        json!({ "site": SITE, "tournament": "777" }),
    )
    .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = organizer(
        other_path.clone(),
        json!({ "site": SITE, "tournament": "9001" }),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    let (status, _) = stop_event(&bridge, &player1, &other).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(events().await["events"].as_array().unwrap().len(), 1);
}

/// The value of cookie `name` that `response` sets.
fn set_cookie(response: &reqwest::Response, name: &str) -> Option<String> {
    response
        .headers()
        .get_all("set-cookie")
        .iter()
        .filter_map(|value| value.to_str().ok())
        .find_map(|value| value.strip_prefix(&format!("{name}=")))
        .map(|rest| rest.split(';').next().unwrap_or_default().to_owned())
}

/// Signs in to the web desk with `code`, from a browser that keeps the
/// sign-in's cookie or not; the callback's answer.
async fn web_sign_in(bridge: &Bridge, code: &str, same_browser: bool) -> reqwest::Response {
    let started = bridge
        .client
        .get(bridge.url("/web/sign-in/challonge"))
        .send()
        .await
        .unwrap();
    assert_eq!(started.status(), StatusCode::SEE_OTHER);
    let location = url::Url::parse(started.headers()["location"].to_str().unwrap()).unwrap();
    let state = sign_in::query(&location)["state"].clone();
    assert!(state.starts_with("web."));
    let kept = set_cookie(&started, "ember_desk_state").unwrap();
    let back = bridge.client.get(bridge.url(&format!(
        "{}?code={code}&state={state}",
        ember_protocol::sites::callback_path(SITE)
    )));
    let back = if same_browser {
        back.header("cookie", format!("ember_desk_state={kept}"))
    } else {
        back
    };
    back.send().await.unwrap()
}

// An organizer signs in to the web desk with the Challonge account their
// Ember ID holds. The cookie works the organizer routes, a change needs the
// session's CSRF token, and an account no Ember ID holds signs nobody in.
#[tokio::test]
async fn an_organizer_signs_in_to_the_web_desk_with_a_linked_account() {
    let fake = fake_challonge();
    fake.lists("/v2.1/tournaments.json", |_| json!({ "data": [] }));
    let bridge = desk_bridge(&fake).await;
    let player1 = player(&bridge, 1).await;
    CHALLONGE.connect(&bridge, &player1, "good-player1").await;

    // Only the browser that started it finishes it.
    let back = web_sign_in(&bridge, "good-player1", false).await;
    assert_eq!(back.status(), StatusCode::OK);
    assert!(set_cookie(&back, "ember_desk").is_none());
    assert!(back.text().await.unwrap().contains("Sign-in expired"));

    // An account no Ember ID holds is linked from the game first.
    let back = web_sign_in(&bridge, "good-player2", true).await;
    assert!(set_cookie(&back, "ember_desk").is_none());
    assert!(
        back.text()
            .await
            .unwrap()
            .contains("Link your account in Ember first")
    );

    let back = web_sign_in(&bridge, "good-player1", true).await;
    assert_eq!(back.status(), StatusCode::SEE_OTHER);
    assert_eq!(back.headers()["location"], "/desk");
    let desk = format!("ember_desk={}", set_cookie(&back, "ember_desk").unwrap());
    let with_desk = |request: reqwest::RequestBuilder| request.header("cookie", desk.clone());

    let session: Json = serde_json::from_str(
        &with_desk(bridge.client.get(bridge.url("/web/session")))
            .send()
            .await
            .unwrap()
            .text()
            .await
            .unwrap(),
    )
    .unwrap();
    assert_eq!(session["ember_id"], player1.id().as_str());
    let csrf = session["csrf"].as_str().unwrap().to_owned();

    // Reads take the cookie; changes need its CSRF token too.
    let listed = with_desk(bridge.client.get(bridge.url("/v1/organizer/events")))
        .send()
        .await
        .unwrap();
    assert_eq!(listed.status(), StatusCode::OK);
    let draft = || {
        with_desk(bridge.client.post(bridge.url("/v1/organizer/drafts")))
            .header("content-type", "application/json")
            .body(json!({ "title": "Web Cup" }).to_string())
    };
    assert_eq!(
        draft().send().await.unwrap().status(),
        StatusCode::FORBIDDEN
    );
    let wrong = draft().header("x-ember-csrf", "nope").send().await.unwrap();
    assert_eq!(wrong.status(), StatusCode::FORBIDDEN);
    let made = draft().header("x-ember-csrf", &csrf).send().await.unwrap();
    assert_eq!(made.status(), StatusCode::CREATED);
    let (_, events) = bridge.get(player1.token(), "/v1/organizer/events").await;
    assert_eq!(events["events"][0]["title"], "Web Cup");

    // Signed out, the cookie works nothing.
    let out = with_desk(bridge.client.post(bridge.url("/web/sign-out")))
        .header("x-ember-csrf", &csrf)
        .send()
        .await
        .unwrap();
    assert_eq!(out.status(), StatusCode::OK);
    let gone = with_desk(bridge.client.get(bridge.url("/web/session")))
        .send()
        .await
        .unwrap();
    assert_eq!(gone.status(), StatusCode::UNAUTHORIZED);
}
