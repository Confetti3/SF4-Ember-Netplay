//! Challonge sign-in against a stand-in Challonge: starting needs a proof and
//! gives the code to show, the bridge polls no faster than it may and ends a
//! sign-in the way Challonge answers, an account another Ember ID holds moves
//! only when the player says so, cancelling, a newer start or a disconnect
//! ends what was in flight, and connections that may find players by
//! Challonge username get them linked. No token or device code leaves the
//! bridge.
mod common;
#[path = "challonge/support.rs"]
mod support;

use common::{Bridge, code};
use ember_bridge::integrations::Secrets;
use ember_protocol::challenge::{Action, Method};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use support::*;
use zeroize::Zeroizing;

#[tokio::test]
async fn a_player_connects_challonge_by_entering_the_code() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    let features = capabilities["features"].as_array().unwrap();
    assert!(
        features.contains(&json!("challonge")) && features.contains(&json!("challonge.accounts"))
    );
    let kate = player(&bridge, 1).await;
    assert_eq!(
        connection(&bridge, &kate).await,
        json!({ "account": null, "sign_in": null })
    );

    let (status, started) = try_start(&bridge, &kate).await;
    assert_eq!(status, StatusCode::CREATED, "{started}");
    let now = bridge.clock.now();
    assert_eq!(
        started,
        json!({
            "user_code": "CODE01",
            "verification_uri": "https://auth.challonge.com/oauth/device",
            "verification_uri_complete": "https://auth.challonge.com/oauth/device?user_code=CODE01",
            "expires_at": now + 300,
        })
    );
    let asked = &fake.seen("/v2.1/oauth/authorize_device")[0];
    assert_eq!(asked.params["client_id"], CLIENT_ID);
    assert_eq!(asked.params["scope"], "me");
    assert_eq!(asked.form, asked.params);
    let waiting = connection(&bridge, &kate).await;
    assert_eq!(waiting["sign_in"]["state"], "waiting");
    assert_eq!(waiting["sign_in"]["user_code"], "CODE01");

    // Challonge asks for a poll every 2 seconds; the bridge waits 5.
    poll_after(&bridge, 4).await;
    assert!(fake.seen("/oauth/token").is_empty());
    fake.answer("CODE01", &[Answer::Pending, Answer::Approve(KATE)]);
    poll_after(&bridge, 1).await;
    assert_eq!(
        sign_in_state(&bridge, &kate).await.as_deref(),
        Some("waiting")
    );
    poll_after(&bridge, 5).await;
    let connected = connection(&bridge, &kate).await;
    assert_eq!(connected["sign_in"], Json::Null, "{connected}");
    assert_eq!(connected["account"]["user_id"], KATE.0);
    assert_eq!(connected["account"]["username"], KATE.1);

    let polls = fake.seen("/oauth/token");
    assert_eq!(polls.len(), 2);
    let poll = &polls[0];
    assert_eq!(
        poll.params["grant_type"],
        "urn:ietf:params:oauth:grant-type:device_code"
    );
    assert_eq!(poll.params["device_code"], fake.device_code("CODE01"));
    assert_eq!(poll.params["client_id"], CLIENT_ID);
    assert!(!poll.params.contains_key("client_secret"));
    assert_eq!(poll.form, poll.params);
    let me = &fake.seen("/v2.1/me.json")[0];
    assert_eq!(me.headers["authorization-type"], "v2");
    assert_eq!(
        me.headers["authorization"],
        format!("Bearer {}", fake.tokens()[0])
    );

    // The device code and the token went nowhere but Challonge: not into an
    // answer, and the token not into the database.
    let device_code = fake.device_code("CODE01");
    for answer in [started.to_string(), connected.to_string()] {
        assert!(!answer.contains(&device_code) && !answer.contains("access-token"));
    }
    assert!(!database_holds(&bridge, "access-token").await);
    assert!(!database_holds(&bridge, "refresh-token").await);
}

#[tokio::test]
async fn a_client_secret_goes_with_each_poll_when_there_is_one() {
    let fake = Challonge::default();
    let bridge = bridge_with(
        Some(&fake),
        json!({}),
        Secrets {
            challonge_client_secret: Some(Zeroizing::new("challonge-secret".into())),
            ..Default::default()
        },
    )
    .await;
    let kate = player(&bridge, 1).await;
    connect(&bridge, &fake, &kate, SAM).await;
    assert_eq!(account(&bridge, &kate).await.as_deref(), Some(SAM.0));
    let poll = &fake.seen("/oauth/token")[0];
    assert_eq!(poll.params["client_secret"], "challonge-secret");
    // Not to the device authorization, which takes none.
    assert!(
        !fake.seen("/v2.1/oauth/authorize_device")[0]
            .params
            .contains_key("client_secret")
    );
}

#[tokio::test]
async fn challonges_answers_end_a_sign_in_or_slow_it_down() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let mut kate = player(&bridge, 1).await;

    // slow_down adds 5 seconds to the interval, and so does a rate limit
    // answer; a server error is waited out.
    let user_code = start(&bridge, &kate).await;
    fake.answer(
        &user_code,
        &[Answer::SlowDown, Answer::ServerError, Answer::Pending],
    );
    poll_after(&bridge, 5).await;
    poll_after(&bridge, 9).await;
    assert_eq!(fake.seen("/oauth/token").len(), 1);
    poll_after(&bridge, 1).await;
    assert_eq!(fake.seen("/oauth/token").len(), 2);
    assert_eq!(
        sign_in_state(&bridge, &kate).await.as_deref(),
        Some("waiting")
    );
    poll_after(&bridge, 10).await;
    assert_eq!(fake.seen("/oauth/token").len(), 3);

    for (answer, ended) in [
        (Answer::Denied, "denied"),
        (Answer::ExpiredToken, "expired"),
        (Answer::InvalidGrant, "expired"),
        (Answer::Unsupported, "failed"),
    ] {
        let user_code = start(&bridge, &kate).await;
        fake.answer(&user_code, &[answer]);
        poll_after(&bridge, 5).await;
        let sign_in = connection(&bridge, &kate).await["sign_in"].clone();
        assert_eq!(sign_in["state"], ended, "{answer:?}");
        assert_eq!(sign_in["user_code"], user_code.as_str());
        // An ended sign-in is not polled again.
        let polls = fake.seen("/oauth/token").len();
        poll_after(&bridge, 60).await;
        assert_eq!(fake.seen("/oauth/token").len(), polls);
    }
    assert_eq!(account(&bridge, &kate).await, None);

    // Approved, but Challonge does not say who signed in.
    fake.fail_me(true);
    let user_code = start(&bridge, &kate).await;
    fake.answer(&user_code, &[Answer::Approve(KATE)]);
    poll_after(&bridge, 5).await;
    assert_eq!(
        sign_in_state(&bridge, &kate).await.as_deref(),
        Some("failed")
    );
    assert_eq!(account(&bridge, &kate).await, None);
    fake.fail_me(false);

    // Never approved: expired once the code lapses, then gone.
    let user_code = start(&bridge, &kate).await;
    bridge.clock.advance(300);
    assert_eq!(
        sign_in_state(&bridge, &kate).await.as_deref(),
        Some("expired")
    );
    let polls = fake.seen("/oauth/token").len();
    fake.answer(&user_code, &[Answer::Approve(KATE)]);
    ember_bridge::poll_challonge(bridge.state()).await;
    ember_bridge::maintain(bridge.state()).await;
    assert_eq!(fake.seen("/oauth/token").len(), polls);
    assert_eq!(
        sign_in_state(&bridge, &kate).await.as_deref(),
        Some("expired")
    );
    bridge.clock.advance(10 * 60);
    ember_bridge::maintain(bridge.state()).await;
    // A session lasts ten minutes too.
    bridge.open_session(&mut kate).await;
    assert_eq!(sign_in_state(&bridge, &kate).await, None);
    assert_eq!(account(&bridge, &kate).await, None);
}

#[tokio::test]
async fn a_start_needs_a_proof_challonges_pages_and_a_free_hour() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let mut kate = player(&bridge, 1).await;

    // A proof made for another action starts nothing.
    let body = bridge
        .prove(
            &kate,
            Action::ChallongeCancel,
            Method::Delete,
            "/v1/challonge/start",
            json!({}),
        )
        .await;
    let (status, body) = bridge
        .send_proof(&kate, Method::Post, "/v1/challonge/start", body)
        .await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::UNAUTHORIZED, "invalid_signature"),
        "{body}"
    );
    assert_eq!(connection(&bridge, &kate).await["sign_in"], Json::Null);

    // A page that is not Challonge's own over https fails the start.
    for page in [
        "http://auth.challonge.com/oauth/device",
        "https://challonge.example/oauth/device",
    ] {
        fake.next_page(page);
        let (status, body) = try_start(&bridge, &kate).await;
        assert_eq!(
            (status, code(&body)),
            (StatusCode::SERVICE_UNAVAILABLE, "service_unavailable"),
            "{body}"
        );
        assert_eq!(connection(&bridge, &kate).await["sign_in"], Json::Null);
    }
    fake.refuse_devices(true);
    let (status, _) = try_start(&bridge, &kate).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
    fake.refuse_devices(false);

    // Ten starts an hour, counting those that failed.
    for _ in 4..10 {
        start(&bridge, &kate).await;
    }
    let (status, body) = try_start(&bridge, &kate).await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::TOO_MANY_REQUESTS, "rate_limited"),
        "{body}"
    );
    let sam = player(&bridge, 2).await;
    start(&bridge, &sam).await;
    bridge.clock.advance(3600);
    bridge.open_session(&mut kate).await;
    start(&bridge, &kate).await;
}

#[tokio::test]
async fn only_the_latest_sign_in_connects_and_the_player_can_end_it() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let kate = player(&bridge, 1).await;

    // A newer start replaces the first: the first code is not polled again,
    // even once Challonge would approve it.
    let first = start(&bridge, &kate).await;
    let second = start(&bridge, &kate).await;
    fake.answer(&first, &[Answer::Approve(SAM)]);
    poll_after(&bridge, 5).await;
    let polls = fake.seen("/oauth/token");
    assert_eq!(polls.len(), 1);
    assert_eq!(polls[0].params["device_code"], fake.device_code(&second));
    assert_eq!(account(&bridge, &kate).await, None);

    // Cancelled: nothing is polled or connected after.
    let (status, body) = cancel(&bridge, &kate).await;
    assert_eq!((status, body), (StatusCode::OK, json!({})));
    assert_eq!(connection(&bridge, &kate).await["sign_in"], Json::Null);
    fake.answer(&second, &[Answer::Approve(KATE)]);
    poll_after(&bridge, 5).await;
    assert_eq!(fake.seen("/oauth/token").len(), 1);
    assert_eq!(account(&bridge, &kate).await, None);

    // A disconnect ends the sign-in in flight too.
    connect(&bridge, &fake, &kate, KATE).await;
    let user_code = start(&bridge, &kate).await;
    let (status, removed) = disconnect(&bridge, &kate).await;
    assert_eq!(
        (status, removed),
        (StatusCode::OK, json!({ "account": null, "sign_in": null }))
    );
    fake.answer(&user_code, &[Answer::Approve(KATE)]);
    poll_after(&bridge, 5).await;
    assert_eq!(
        connection(&bridge, &kate).await,
        json!({ "account": null, "sign_in": null })
    );
}

#[tokio::test]
async fn an_account_moves_only_from_an_unused_ember_id_and_when_the_player_says_so() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let (one, mut two) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &fake, &one, KATE).await;
    assert_eq!(found(&bridge, BRACKETS, &["kate_99"]).await.len(), 1);
    assert!(link_of(&bridge, BRACKETS, &one).await.is_some());

    // Signed in from another Ember ID while the first is in use: held there,
    // and nothing to choose.
    let held = sign_in_held_elsewhere(&bridge, &fake, &two, KATE).await;
    assert_eq!(held["state"], "held");
    assert_eq!(held["username"], KATE.1);
    assert_eq!(account(&bridge, &two).await, None);
    let (status, body) = choose(&bridge, &two, "move").await;
    assert_eq!((status, code(&body)), (StatusCode::NOT_FOUND, "not_found"));
    assert_eq!(account(&bridge, &one).await.as_deref(), Some(KATE.0));
    // A session a day short of the idle time still counts as use.
    bridge
        .clock
        .advance(ember_protocol::challonge::MOVE_IDLE_SECS as i64 - 24 * 3600);
    bridge.open_session(&mut two).await;
    let held = sign_in_held_elsewhere(&bridge, &fake, &two, KATE).await;
    assert_eq!(held["state"], "held");

    // Once the first has gone unused that long, the sign-in waits for the
    // player.
    bridge.clock.advance(24 * 3600);
    bridge.open_session(&mut two).await;
    let moving = sign_in_held_elsewhere(&bridge, &fake, &two, KATE).await;
    assert_eq!(moving["state"], "moving");
    assert_eq!(moving["username"], KATE.1);
    assert_eq!(
        holder(&bridge, KATE).await.as_deref(),
        Some(one.id().as_str())
    );

    // Kept where it is: the sign-in ends and nothing changes.
    let (status, kept) = choose(&bridge, &two, "keep").await;
    assert_eq!(
        (status, kept),
        (StatusCode::OK, json!({ "account": null, "sign_in": null }))
    );
    assert_eq!(
        holder(&bridge, KATE).await.as_deref(),
        Some(one.id().as_str())
    );
    let (status, body) = choose(&bridge, &two, "move").await;
    assert_eq!((status, code(&body)), (StatusCode::NOT_FOUND, "not_found"));

    // Moved: the link it approved for the first Ember ID ends, and the next
    // lookup links it where it now is.
    let moving = sign_in_held_elsewhere(&bridge, &fake, &two, KATE).await;
    assert_eq!(moving["state"], "moving");
    let (status, moved) = choose(&bridge, &two, "move").await;
    assert_eq!(status, StatusCode::OK, "{moved}");
    assert_eq!(moved["account"]["user_id"], KATE.0);
    assert_eq!(moved["sign_in"], Json::Null);
    assert_eq!(
        holder(&bridge, KATE).await.as_deref(),
        Some(two.id().as_str())
    );
    assert_eq!(link_of(&bridge, BRACKETS, &one).await, None);
    let found = found(&bridge, BRACKETS, &["KATE_99"]).await;
    assert_eq!(found[0].2, two.id().as_str());

    // A choice that is not move or keep is refused.
    let (status, _) = choose(&bridge, &two, "maybe").await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
}

#[tokio::test]
async fn a_move_holds_only_while_the_account_stays_where_it_was() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let (mut one, mut two) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &fake, &one, KATE).await;
    go_idle(&bridge);
    bridge.open_session(&mut two).await;
    let moving = sign_in_held_elsewhere(&bridge, &fake, &two, KATE).await;
    assert_eq!(moving["state"], "moving");

    // The first Ember ID comes back and disconnects it meanwhile.
    bridge.open_session(&mut one).await;
    disconnect(&bridge, &one).await;
    let (status, body) = choose(&bridge, &two, "move").await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::CONFLICT, "link_conflict"),
        "{body}"
    );
    assert_eq!(
        connection(&bridge, &two).await,
        json!({ "account": null, "sign_in": null })
    );
    assert_eq!(account(&bridge, &one).await, None);

    // A move nobody answered lapses with the code.
    connect(&bridge, &fake, &one, KATE).await;
    go_idle(&bridge);
    bridge.open_session(&mut two).await;
    let moving = sign_in_held_elsewhere(&bridge, &fake, &two, KATE).await;
    assert_eq!(moving["state"], "moving");
    bridge.clock.advance(300);
    ember_bridge::maintain(bridge.state()).await;
    let lapsed = connection(&bridge, &two).await["sign_in"].clone();
    assert_eq!(lapsed["state"], "expired");
    assert_eq!(lapsed.get("username"), None);
    let (status, _) = choose(&bridge, &two, "move").await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    assert_eq!(
        holder(&bridge, KATE).await.as_deref(),
        Some(one.id().as_str())
    );
}

#[tokio::test]
async fn a_move_fails_if_the_ember_id_holding_the_account_is_used_again() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let (mut one, mut two) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &fake, &one, KATE).await;
    go_idle(&bridge);
    bridge.open_session(&mut two).await;
    let moving = sign_in_held_elsewhere(&bridge, &fake, &two, KATE).await;
    assert_eq!(moving["state"], "moving");

    // Its owner opens Ember before the player chooses.
    bridge.open_session(&mut one).await;
    let (status, body) = choose(&bridge, &two, "move").await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::CONFLICT, "link_conflict"),
        "{body}"
    );
    let held = connection(&bridge, &two).await;
    assert_eq!(held["account"], Json::Null);
    assert_eq!(held["sign_in"]["state"], "held");
    assert_eq!(held["sign_in"]["username"], KATE.1);
    assert_eq!(account(&bridge, &one).await.as_deref(), Some(KATE.0));
    // Disconnected there, the account connects here with the next sign-in.
    disconnect(&bridge, &one).await;
    connect(&bridge, &fake, &two, KATE).await;
    assert_eq!(account(&bridge, &two).await.as_deref(), Some(KATE.0));
}

#[tokio::test]
async fn the_bridge_keeps_to_its_waiting_and_monthly_limits() {
    let fake = Challonge::default();
    let bridge = bridge_with(
        Some(&fake),
        json!({ "max_waiting_sign_ins": 2, "monthly_request_budget": 6 }),
        Secrets::default(),
    )
    .await;
    // The first second of a month, UTC (2026-11-01).
    let month_start: i64 = 1_793_491_200;
    bridge
        .clock
        .advance(month_start - bridge.clock.now() as i64);
    let players = [
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
    ];

    // Two sign-ins wait at once; a third start is refused until one ends,
    // though a player may start their own again.
    start(&bridge, &players[0]).await;
    start(&bridge, &players[1]).await;
    let (status, body) = try_start(&bridge, &players[2]).await;
    assert_eq!(
        (status, code(&body)),
        (StatusCode::SERVICE_UNAVAILABLE, "service_unavailable"),
        "{body}"
    );
    start(&bridge, &players[1]).await;
    cancel(&bridge, &players[1]).await;
    start(&bridge, &players[2]).await;
    // Four device codes so far; the refused start asked Challonge nothing.
    assert_eq!(fake.seen("/v2.1/oauth/authorize_device").len(), 4);

    // Two polls spend the budget's last requests and cross 80% of it.
    poll_after(&bridge, 5).await;
    assert_eq!(fake.seen("/oauth/token").len(), 2);
    assert_eq!(requests_spent(&bridge).await, (6, true));
    // Then waiting sign-ins end failed instead of polling, and starts are
    // refused.
    poll_after(&bridge, 5).await;
    assert_eq!(fake.seen("/oauth/token").len(), 2);
    for player in [&players[0], &players[2]] {
        assert_eq!(
            sign_in_state(&bridge, player).await.as_deref(),
            Some("failed")
        );
    }
    let (status, _) = try_start(&bridge, &players[1]).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);

    // The next month starts afresh.
    bridge.clock.advance(30 * 24 * 3600);
    let kate = player(&bridge, 4).await;
    connect(&bridge, &fake, &kate, KATE).await;
    assert_eq!(account(&bridge, &kate).await.as_deref(), Some(KATE.0));
    // A device code, a poll and /me.
    assert_eq!(requests_spent(&bridge).await, (3, false));
}

/// This month's Challonge requests, and whether the 80% warning was logged.
async fn requests_spent(bridge: &Bridge) -> (u64, bool) {
    let now = bridge.clock.now();
    bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT spent, warned FROM challonge_requests WHERE month = strftime('%Y-%m', ?1, 'unixepoch')",
                [now],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?)
        })
        .await
        .unwrap()
}

#[tokio::test]
async fn the_latest_sign_in_replaces_an_ember_ids_account() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let kate = player(&bridge, 1).await;
    connect(&bridge, &fake, &kate, KATE).await;
    found(&bridge, BRACKETS, &[KATE.1]).await;
    assert_eq!(
        link_of(&bridge, BRACKETS, &kate).await,
        Some(("challonge".into(), format!("challonge:{}", KATE.0)))
    );
    // Another account for the same Ember ID ends the first one's links.
    connect(&bridge, &fake, &kate, SAM).await;
    assert_eq!(account(&bridge, &kate).await.as_deref(), Some(SAM.0));
    assert_eq!(link_of(&bridge, BRACKETS, &kate).await, None);
    assert_eq!(found(&bridge, BRACKETS, &[KATE.1]).await, []);
    assert_eq!(found(&bridge, BRACKETS, &[SAM.1]).await.len(), 1);
}

#[tokio::test]
async fn a_lookup_finds_connected_players_by_username_without_case() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &fake, &kate, KATE).await;
    connect(&bridge, &fake, &sam, SAM).await;
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
            Some(("challonge".into(), format!("challonge:{}", user.0)))
        );
    }
    let events = bridge.events(&provider, "0").await;
    let completed: Vec<_> = events[after..]
        .iter()
        .filter(|event| event["type"] == "io.ember.tournament.identity.link.completed.v1")
        .map(|event| event["data"]["approved_via"].clone())
        .collect();
    assert_eq!(completed, [json!("challonge"), json!("challonge")]);
    // Asked again, the same participants.
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
async fn each_list_needs_its_own_setting() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let kate = player(&bridge, 1).await;
    connect(&bridge, &fake, &kate, KATE).await;
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
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &fake, &kate, KATE).await;
    connect(&bridge, &fake, &sam, SAM).await;
    let discord_kate = "274220342558756145";
    let ember_id = kate.id().as_str().to_owned();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, 'kate', 1)",
                [discord_kate, &ember_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    let (status, answer) = lookup(&bridge, BOTH, &[KATE.1, SAM.1], &[discord_kate]).await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    // Kate is linked once, by Discord, which was asked first; both answers
    // name the same participant.
    let discord_found = &answer["players"][0];
    let challonge_found = answer["challonge"].as_array().unwrap();
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
    assert_eq!(link_of(&bridge, BOTH, &sam).await.unwrap().0, "challonge");
}

#[tokio::test]
async fn a_link_the_player_removed_is_not_made_again_until_a_new_sign_in() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &fake, &kate, KATE).await;
    connect(&bridge, &fake, &sam, SAM).await;
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
    connect(&bridge, &fake, &kate, KATE).await;
    assert_eq!(found(&bridge, BRACKETS, &[KATE.1]).await.len(), 1);
}

#[tokio::test]
async fn the_most_recently_connected_account_answers_for_a_shared_username() {
    let fake = Challonge::default();
    let bridge = bridge(&fake).await;
    let (old, new) = (player(&bridge, 1).await, player(&bridge, 2).await);
    // Two Challonge accounts that had the same username at different times.
    for (user_id, player, connected_at) in [("2001", &old, 100), ("2002", &new, 200)] {
        let ember_id = player.id().as_str().to_owned();
        bridge
            .state()
            .db
            .write(move |tx| {
                tx.execute(
                    "INSERT INTO challonge_accounts (user_id, ember_id, username, username_key, connected_at)
                     VALUES (?1, ?2, 'Renamed', 'renamed', ?3)",
                    rusqlite::params![user_id, ember_id, connected_at],
                )?;
                Ok(())
            })
            .await
            .unwrap();
    }
    let answer = found(&bridge, BRACKETS, &["RENAMED"]).await;
    assert_eq!(
        answer,
        [(
            "RENAMED".to_owned(),
            "2002".to_owned(),
            new.id().to_string()
        )]
    );
}

#[tokio::test]
async fn a_bridge_without_challonge_still_lets_a_player_withdraw() {
    let fake = Challonge::default();
    let bridge = bridge_with(None, json!({}), Secrets::default()).await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    let features = capabilities["features"].as_array().unwrap();
    assert!(features.contains(&json!("challonge.accounts")));
    assert!(!features.contains(&json!("challonge")));
    let kate = player(&bridge, 1).await;
    let ember_id = kate.id().as_str().to_owned();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO challonge_accounts (user_id, ember_id, username, username_key, connected_at)
                 VALUES ('1001', ?1, 'Kate_99', 'kate_99', 1)",
                [&ember_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();

    let (status, _) = try_start(&bridge, &kate).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = choose(&bridge, &kate, "move").await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    assert_eq!(cancel(&bridge, &kate).await.0, StatusCode::OK);
    assert_eq!(account(&bridge, &kate).await.as_deref(), Some("1001"));
    assert_eq!(disconnect(&bridge, &kate).await.0, StatusCode::OK);
    assert_eq!(account(&bridge, &kate).await, None);
    // The poller is not running, and nothing asked Challonge.
    ember_bridge::poll_challonge(bridge.state()).await;
    assert!(fake.seen("/oauth/token").is_empty());
}

/// Whether `text` is anywhere in the bridge's database files.
async fn database_holds(bridge: &Bridge, text: &str) -> bool {
    let database = bridge.state().config.database.clone();
    ["", "-wal"].iter().any(|suffix| {
        let mut path = database.clone().into_os_string();
        path.push(suffix);
        std::fs::read(path).is_ok_and(|bytes| {
            bytes
                .windows(text.len())
                .any(|window| window == text.as_bytes())
        })
    })
}
