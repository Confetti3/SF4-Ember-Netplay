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

use common::{Bridge, code};
use ember_bridge::integrations::Secrets;
use ember_protocol::challenge::{Action, Method};
use reqwest::StatusCode;
use serde_json::json;
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
    let kate = player(&bridge, 1).await;
    assert_eq!(account(&bridge, &kate).await, None);

    let (status, started) = try_start(&bridge, &kate).await;
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
    let page = back(&bridge, &format!("code=good-kate&state={}", query["state"])).await;
    assert!(
        page.contains("Connect this Challonge account?")
            && page.contains(KATE.1)
            && page.contains(&kate.id().fingerprint())
            && page.contains("Cups")
            && page.contains("action=\"/v1/sites/challonge/callback\""),
        "{page}"
    );
    assert_eq!(account(&bridge, &kate).await, None);
    let page = decide(&bridge, &confirmation(&page), "connect").await;
    assert!(
        page.contains("Challonge connected") && page.contains(KATE.1),
        "{page}"
    );
    let (_, connected) = bridge.get(kate.token(), "/v1/sites/challonge").await;
    assert_eq!(connected["account"]["user_id"], KATE.0);
    assert_eq!(connected["account"]["username"], KATE.1);

    // The code is exchanged in a form body only, with the client secret.
    let exchange = &fake.seen("/oauth/token")[0];
    assert!(exchange.query.is_empty(), "{:?}", exchange.query);
    assert_eq!(exchange.form["grant_type"], "authorization_code");
    assert_eq!(exchange.form["code"], "good-kate");
    assert_eq!(exchange.form["client_id"], CLIENT_ID);
    assert_eq!(exchange.form["client_secret"], CLIENT_SECRET);
    assert_eq!(
        exchange.form["redirect_uri"],
        bridge.url("/v1/sites/challonge/callback")
    );
    let me = &fake.seen("/v2.1/me.json")[0];
    assert_eq!(me.headers["authorization-type"], "v2");
    assert_eq!(me.headers["accept"], "application/json");
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
    connect(&bridge, &one, "kate").await;
    assert_eq!(found(&bridge, BRACKETS, &["kate_99"]).await.len(), 1);

    // The page names both Ember IDs; keeping it changes nothing.
    let page = sign_in(&bridge, &two, "good-kate").await;
    assert!(
        page.contains("Move this Challonge account?")
            && page.contains(&one.id().fingerprint())
            && page.contains(&two.id().fingerprint()),
        "{page}"
    );
    let kept = decide(&bridge, &confirmation(&page), "keep").await;
    assert!(kept.contains("Nothing moved"), "{kept}");
    assert_eq!(account(&bridge, &one).await.as_deref(), Some(KATE.0));
    assert_eq!(account(&bridge, &two).await, None);

    // Moved: the link it approved for the first Ember ID ends, and the next
    // lookup links it where it now is.
    let page = sign_in(&bridge, &two, "good-kate").await;
    let moved = decide(&bridge, &confirmation(&page), "move").await;
    assert!(moved.contains("Challonge connected"), "{moved}");
    assert_eq!(account(&bridge, &one).await, None);
    assert_eq!(account(&bridge, &two).await.as_deref(), Some(KATE.0));
    assert_eq!(link_of(&bridge, BRACKETS, &one).await, None);
    assert_eq!(
        found(&bridge, BRACKETS, &["KATE_99"]).await[0].2,
        two.id().as_str()
    );

    // A move page left open while the account moved again changes nothing.
    let page = sign_in(&bridge, &one, "good-kate").await;
    disconnect(&bridge, &two).await;
    let changed = decide(&bridge, &confirmation(&page), "move").await;
    assert!(changed.contains("after this page asked"), "{changed}");
    assert_eq!(account(&bridge, &one).await, None);
}

#[tokio::test]
async fn the_latest_sign_in_replaces_an_ember_ids_account() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let kate = player(&bridge, 1).await;
    connect(&bridge, &kate, "kate").await;
    found(&bridge, BRACKETS, &[KATE.1]).await;
    assert_eq!(
        link_of(&bridge, BRACKETS, &kate).await,
        Some(("site".into(), format!("challonge:{}", KATE.0)))
    );
    // Another account for the same Ember ID: the page says what it replaces,
    // and connecting it ends the first one's links.
    let page = sign_in(&bridge, &kate, "good-sam").await;
    assert!(
        page.contains("is connected to Challonge account <strong>Kate_99</strong> now"),
        "{page}"
    );
    decide(&bridge, &confirmation(&page), "connect").await;
    assert_eq!(account(&bridge, &kate).await.as_deref(), Some(SAM.0));
    assert_eq!(link_of(&bridge, BRACKETS, &kate).await, None);
    assert_eq!(found(&bridge, BRACKETS, &[KATE.1]).await, []);
    assert_eq!(found(&bridge, BRACKETS, &[SAM.1]).await.len(), 1);
}

#[tokio::test]
async fn challonge_refusing_or_failing_connects_nothing() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let kate = player(&bridge, 1).await;
    for (code, says) in [
        ("busy", "Challonge did not answer"),
        (
            "odd-name",
            "username <strong>has space</strong> cannot be used to find you",
        ),
        ("made-up", "Challonge did not answer"),
    ] {
        let page = sign_in(&bridge, &kate, code).await;
        assert!(page.contains(says), "{code}: {page}");
    }
    let state = start(&bridge, &kate).await;
    let page = back(&bridge, &format!("error=access_denied&state={state}")).await;
    assert!(
        page.contains("Challonge did not connect your account"),
        "{page}"
    );
    // Each sign-in is over: its state answers nothing again.
    let page = back(&bridge, &format!("code=good-kate&state={state}")).await;
    assert!(page.contains("Sign-in expired"), "{page}");
    assert_eq!(account(&bridge, &kate).await, None);
    // A 429 is one of the exchanges Challonge saw.
    assert_eq!(fake.seen("/oauth/token").len(), 3);
}

#[tokio::test]
async fn a_second_answer_or_a_cancel_while_challonge_checks_wins_nothing() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let kate = player(&bridge, 1).await;

    // Reloaded while Challonge still checks the first answer.
    let state = start(&bridge, &kate).await;
    let url = bridge.url(&format!(
        "/v1/sites/challonge/callback?code=held-kate&state={state}"
    ));
    let client = bridge.client.clone();
    let first =
        tokio::spawn(async move { client.get(url).send().await.unwrap().text().await.unwrap() });
    HELD.arrived.notified().await;
    let page = back(&bridge, &format!("code=held-kate&state={state}")).await;
    assert!(page.contains("Sign-in expired"), "{page}");
    HELD.release.notify_one();
    let page = first.await.unwrap();
    assert!(page.contains("Connect this Challonge account?"), "{page}");

    // Cancelled in Ember while Challonge checks: the answer connects nothing.
    let state = start(&bridge, &kate).await;
    let url = bridge.url(&format!(
        "/v1/sites/challonge/callback?code=held-kate&state={state}"
    ));
    let client = bridge.client.clone();
    let late =
        tokio::spawn(async move { client.get(url).send().await.unwrap().text().await.unwrap() });
    HELD.arrived.notified().await;
    assert_eq!(cancel(&bridge, &kate).await.0, StatusCode::OK);
    HELD.release.notify_one();
    let page = late.await.unwrap();
    assert!(page.contains("Sign-in expired"), "{page}");
    assert_eq!(account(&bridge, &kate).await, None);

    // Disconnected in Ember while Challonge checks: the same.
    connect(&bridge, &kate, "sam").await;
    let state = start(&bridge, &kate).await;
    let url = bridge.url(&format!(
        "/v1/sites/challonge/callback?code=held-kate&state={state}"
    ));
    let client = bridge.client.clone();
    let late =
        tokio::spawn(async move { client.get(url).send().await.unwrap().text().await.unwrap() });
    HELD.arrived.notified().await;
    assert_eq!(disconnect(&bridge, &kate).await.0, StatusCode::OK);
    HELD.release.notify_one();
    let page = late.await.unwrap();
    assert!(page.contains("Sign-in expired"), "{page}");
    assert_eq!(account(&bridge, &kate).await, None);
}

#[tokio::test]
async fn sign_in_is_off_without_the_client_secret_but_an_account_can_go() {
    let fake = fake_challonge();
    let bridge = bridge_with(&fake, json!({}), Secrets::default()).await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    assert_eq!(capabilities["sign_in_sites"][0]["sign_in"], false);
    let kate = player(&bridge, 1).await;
    let ember_id = kate.id().as_str().to_owned();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO site_accounts (site, user_id, ember_id, username, username_key, connected_at)
                 VALUES ('challonge', '1001', ?1, 'Kate_99', 'kate_99', 1)",
                [&ember_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();

    assert_eq!(try_start(&bridge, &kate).await.0, StatusCode::NOT_FOUND);
    assert!(
        back(&bridge, "code=good-kate&state=x")
            .await
            .contains("Challonge sign-in is off")
    );
    assert_eq!(cancel(&bridge, &kate).await.0, StatusCode::OK);
    assert_eq!(account(&bridge, &kate).await.as_deref(), Some("1001"));
    assert_eq!(
        disconnect(&bridge, &kate).await,
        (StatusCode::OK, json!({ "account": null }))
    );
    assert_eq!(account(&bridge, &kate).await, None);
    assert!(fake.seen("/oauth/token").is_empty());
}

#[tokio::test]
async fn a_start_needs_its_own_proof() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let kate = player(&bridge, 1).await;
    let body = bridge
        .prove(
            &kate,
            Action::SiteCancel,
            Method::Delete,
            "/v1/sites/challonge/start",
            json!({}),
        )
        .await;
    let (status, body) = bridge
        .send_proof(&kate, Method::Post, "/v1/sites/challonge/start", body)
        .await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::UNAUTHORIZED, "invalid_signature"),
        "{body}"
    );
    // There is no move action: the page asks the browser's user instead.
    let (status, _) = bridge
        .post(
            kate.token(),
            "/v1/auth/challenges",
            json!({
                "public_key": kate.identity.public_key(),
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
    let bridge = bridge_with(&fake, json!({ "monthly_request_budget": 4 }), with_secret()).await;
    // The first second of a month, UTC (2026-11-01).
    let month_start: i64 = 1_793_491_200;
    bridge
        .clock
        .advance(month_start - bridge.clock.now() as i64);
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);

    // Two sign-ins started while there is room for both exchanges...
    let kates = start(&bridge, &kate).await;
    let sams = start(&bridge, &sam).await;
    connect_with(&bridge, &kates, "kate").await;
    assert_eq!(requests_spent(&bridge).await, 2);
    // ...but the second exchange spends the last of the budget.
    connect_with(&bridge, &sams, "sam").await;
    assert_eq!(requests_spent(&bridge).await, 4);
    assert_eq!(fake.seen("/oauth/token").len(), 2);
    // Now starts are refused as busy, which Ember tells apart from the
    // service failing.
    let (status, body) = try_start(&bridge, &kate).await;
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
    let mut kate = kate;
    bridge.open_session(&mut kate).await;
    // Signing in again with the account it has connects without asking.
    connect(&bridge, &kate, "kate").await;
    assert_eq!(requests_spent(&bridge).await, 2);
}

#[tokio::test]
async fn throwaway_ember_ids_spend_at_most_a_days_share() {
    let fake = fake_challonge();
    let bridge = bridge_with(
        &fake,
        json!({ "daily_request_budget": 4, "monthly_request_budget": 10 }),
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
        let page = sign_in(&bridge, player, "made-up").await;
        assert!(page.contains("Challonge did not answer"), "{page}");
    }
    assert_eq!(requests_spent(&bridge).await, 4);
    // The day's share is spent: a start is refused as busy.
    let (status, body) = try_start(&bridge, &players[2]).await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::SERVICE_UNAVAILABLE, "sign_in_busy"),
        "{body}"
    );
    // The month keeps the rest for the next days.
    bridge.clock.advance(24 * 3600);
    let mut kate = bridge.player(9);
    bridge.open_session(&mut kate).await;
    connect(&bridge, &kate, "kate").await;
    assert_eq!(requests_spent(&bridge).await, 6);
    assert_eq!(fake.seen("/oauth/token").len(), 3);
}

#[tokio::test]
async fn starts_are_limited_across_the_bridge_too() {
    let fake = fake_challonge();
    let bridge = bridge_with(&fake, json!({ "starts_per_hour": 2 }), with_secret()).await;
    let players = [
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
    ];
    start(&bridge, &players[0]).await;
    start(&bridge, &players[1]).await;
    let (status, body) = try_start(&bridge, &players[2]).await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::SERVICE_UNAVAILABLE, "sign_in_busy"),
        "{body}"
    );
    // An hour later there is room again.
    bridge.clock.advance(3600);
    let mut third = bridge.player(3);
    bridge.open_session(&mut third).await;
    start(&bridge, &third).await;
}

#[tokio::test]
async fn a_sign_in_started_with_room_is_refused_once_the_budget_is_spent() {
    let fake = fake_challonge();
    let bridge = bridge_with(&fake, json!({ "monthly_request_budget": 2 }), with_secret()).await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let kates = start(&bridge, &kate).await;
    let sams = start(&bridge, &sam).await;
    connect_with(&bridge, &sams, "sam").await;
    let page = back(&bridge, &format!("code=good-kate&state={kates}")).await;
    assert!(page.contains("Ember cannot check with Challonge"), "{page}");
    assert_eq!(fake.seen("/oauth/token").len(), 1);
    assert_eq!(account(&bridge, &kate).await, None);
}

/// This month's Challonge requests.
async fn requests_spent(bridge: &Bridge) -> u64 {
    common::sites::requests_spent(bridge, SITE).await
}

#[tokio::test]
async fn a_lookup_finds_connected_players_by_username_without_case() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &kate, "kate").await;
    connect(&bridge, &sam, "sam").await;
    let provider = bridge.provider(BRACKETS).await;
    let after = bridge.events(&provider, "0").await.len();

    // In the order asked, as asked, once each; unknown names are left out.
    let answer = found(&bridge, BRACKETS, &["SAM", "nobody", "kate_99", "Kate_99"]).await;
    assert_eq!(
        answer,
        [
            ("SAM".to_owned(), SAM.0.to_owned(), sam.id().to_string()),
            (
                "kate_99".to_owned(),
                KATE.0.to_owned(),
                kate.id().to_string()
            ),
        ]
    );
    for (player, user) in [(&kate, KATE), (&sam, SAM)] {
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
    let (_, first) = lookup(&bridge, BRACKETS, &["sam"], &[]).await;
    let (_, again) = lookup(&bridge, BRACKETS, &["sam"], &[]).await;
    assert_eq!(first, again);
    assert_eq!(first["players"], json!([]));

    // A name that is not a Challonge username, or too many names.
    for names in [vec!["has space"], vec![""], vec!["kate@example.com"]] {
        let (status, body) = lookup(&bridge, BRACKETS, &names, &[]).await;
        assert_eq!(
            (status, code(&body)),
            (StatusCode::BAD_REQUEST, "invalid_request"),
            "{names:?}"
        );
    }
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
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &kate, "kate").await;
    let connected_at = bridge.clock.now();
    let (_, answer) = lookup(&bridge, BRACKETS, &[KATE.1], &[]).await;
    assert_eq!(answer["accounts"][0]["connected_at"], connected_at);

    // Kate renamed on Challonge, and another account took her old name.
    bridge.clock.advance(60);
    connect(&bridge, &sam, "rival").await;
    let taken = found(&bridge, BOTH, &["kate_99"]).await;
    assert_eq!(
        taken,
        [(
            "kate_99".to_owned(),
            RIVAL.0.to_owned(),
            sam.id().to_string()
        )]
    );
    // Kate's account stays connected; it answers again for the name it
    // next connects with.
    assert_eq!(account(&bridge, &kate).await.as_deref(), Some(KATE.0));
    bridge.clock.advance(60);
    connect(&bridge, &kate, "kate").await;
    assert_eq!(
        found(&bridge, BOTH, &["Kate_99"]).await[0].2,
        kate.id().as_str()
    );
}

#[tokio::test]
async fn a_lookup_that_names_no_list_is_refused() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    for connection in [BRACKETS, DISCORD_ONLY, BOTH] {
        let provider = bridge.provider(connection).await;
        let (status, body) = bridge
            .post(&provider, "/v1/players/lookup", json!({}))
            .await;
        assert_eq!(
            (status, code(&body)),
            (StatusCode::BAD_REQUEST, "invalid_request"),
            "{connection}"
        );
    }
}

#[tokio::test]
async fn each_list_needs_its_own_setting() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let kate = player(&bridge, 1).await;
    connect(&bridge, &kate, "kate").await;
    for (connection, challonge, discord) in [
        (DISCORD_ONLY, vec![KATE.1], vec![]),
        (BRACKETS, vec![KATE.1], vec!["274220342558756145"]),
        ("mock-a", vec![KATE.1], vec![]),
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
    assert_eq!(found(&bridge, BRACKETS, &[KATE.1]).await.len(), 1);
}

#[tokio::test]
async fn discord_and_challonge_find_the_same_player_as_one_link() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &kate, "kate").await;
    connect(&bridge, &sam, "sam").await;
    let discord_kate = "274220342558756145";
    connect_discord(&bridge, discord_kate, &kate).await;
    let (status, answer) = lookup(&bridge, BOTH, &[KATE.1, SAM.1], &[discord_kate]).await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    // Kate is linked once, by Discord, which was asked first; both answers
    // name the same participant.
    let discord_found = &answer["players"][0];
    let challonge_found = answer["accounts"].as_array().unwrap();
    assert_eq!(discord_found["ember_id"], kate.id().as_str());
    assert_eq!(challonge_found.len(), 2);
    assert_eq!(challonge_found[0]["ember_id"], kate.id().as_str());
    assert_eq!(
        challonge_found[0]["participant_id"],
        discord_found["participant_id"]
    );
    assert_eq!(
        link_of(&bridge, BOTH, &kate).await,
        Some(("discord".into(), format!("discord:{discord_kate}")))
    );
    assert_eq!(link_of(&bridge, BOTH, &sam).await.unwrap().0, "site");
}

#[tokio::test]
async fn a_discord_lookup_after_challonge_uses_challonges_link_until_it_ends() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let kate = player(&bridge, 1).await;
    connect(&bridge, &kate, "kate").await;
    let discord_kate = "274220342558756145";
    connect_discord(&bridge, discord_kate, &kate).await;

    // Challonge links her first; Discord then finds the same participant.
    let challonge = found(&bridge, BOTH, &[KATE.1]).await;
    assert_eq!(challonge.len(), 1);
    let (_, by_discord) = lookup(&bridge, BOTH, &[], &[discord_kate]).await;
    let (_, by_challonge) = lookup(&bridge, BOTH, &[KATE.1], &[]).await;
    assert_eq!(
        by_discord["players"][0]["participant_id"],
        by_challonge["accounts"][0]["participant_id"]
    );
    assert_eq!(link_of(&bridge, BOTH, &kate).await.unwrap().0, "site");

    // Disconnecting Challonge ends that link; Discord links her again itself.
    disconnect(&bridge, &kate).await;
    assert_eq!(link_of(&bridge, BOTH, &kate).await, None);
    let (_, again) = lookup(&bridge, BOTH, &[], &[discord_kate]).await;
    assert_eq!(again["players"][0]["ember_id"], kate.id().as_str());
    assert_eq!(
        link_of(&bridge, BOTH, &kate).await,
        Some(("discord".into(), format!("discord:{discord_kate}")))
    );
}

#[tokio::test]
async fn a_link_the_player_removed_is_not_made_again_until_a_new_sign_in() {
    let fake = fake_challonge();
    let bridge = bridge(&fake).await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &kate, "kate").await;
    connect(&bridge, &sam, "sam").await;
    assert_eq!(found(&bridge, BRACKETS, &[KATE.1, SAM.1]).await.len(), 2);

    let (_, links) = bridge.get(kate.token(), "/v1/links").await;
    let link = links["links"][0]["link_id"].as_str().unwrap().to_owned();
    let path = format!("/v1/links/{link}");
    let (status, removed) = send(
        &bridge,
        &kate,
        Action::LinkRemove,
        Method::Delete,
        &path,
        json!({ "link_id": link }),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{removed}");
    for _ in 0..2 {
        let names: Vec<_> = found(&bridge, BRACKETS, &[KATE.1, SAM.1])
            .await
            .into_iter()
            .map(|(name, ..)| name)
            .collect();
        assert_eq!(names, [SAM.1]);
    }
    // Other connections still find her.
    assert_eq!(found(&bridge, BOTH, &[KATE.1]).await.len(), 1);
    // A sign-in afterwards gives the consent back.
    connect(&bridge, &kate, "kate").await;
    assert_eq!(found(&bridge, BRACKETS, &[KATE.1]).await.len(), 1);
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
        found(&bridge, BRACKETS, &["RENAMED"]).await,
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
    let bridge = bridge_with(&fake, json!({ "starts_per_hour": 2 }), with_secret()).await;
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
    start(&bridge, &players[0]).await;
    start(&bridge, &players[1]).await;
    let (status, body) = try_start(&bridge, &players[1]).await;
    assert_eq!(code(&body), "sign_in_busy", "{status}");
}

#[tokio::test]
async fn a_start_that_does_not_commit_gives_its_bridge_wide_slot_back() {
    let fake = fake_challonge();
    let bridge = bridge_with(&fake, json!({ "starts_per_hour": 1 }), with_secret()).await;
    let kate = player(&bridge, 1).await;
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
    let (status, body) = try_start(&bridge, &kate).await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::SERVICE_UNAVAILABLE, "service_unavailable"),
        "{body}"
    );
    sign_ins("ALTER TABLE site_sign_ins_away RENAME TO site_sign_ins")
        .await
        .unwrap();
    // The slot was given back: the next start has it.
    start(&bridge, &kate).await;
    let (_, body) = try_start(&bridge, &kate).await;
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
    let kate = player(&bridge, 1).await;
    let (status, _) = try_start(&bridge, &kate).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
}
