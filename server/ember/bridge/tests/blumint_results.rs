//! What the bridge reports to a stand-in BluMint when a match ends or its
//! score moves: its result, and an abort for a match that ended without one,
//! each with its retries, the score after each game, and the starting wins of
//! a set asked for again. tests/blumint_presence.rs covers the status reports.
mod common;

use common::{
    blumint::*,
    discord as support,
    play::{BUILD, bound, claim, endpoint, permit, report},
    results, seed,
};
use ember_bridge::{config, integrations::Secrets, testing::reconfigured};
use ember_protocol::report::Outcome;
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use zeroize::Zeroizing;

#[tokio::test]
async fn a_completed_match_says_its_result_is_to_be_sent() {
    // Without a key nothing is sent, so the state stays as completion left it.
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "create-1", 1).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 0, "game-1").await;
    let completed: Vec<Json> = f
        .bridge
        .events(&f.provider, "0")
        .await
        .into_iter()
        .filter(|event| event["type"] == "io.ember.tournament.match.completed.v1")
        .collect();
    assert_eq!(completed.len(), 1);
    assert_eq!(completed[0]["data"]["provider_delivery_state"], "queued");
    let (_, snapshot) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(snapshot["provider_delivery_state"], "queued");
}

#[tokio::test]
async fn a_result_is_final_once_it_is_being_sent() {
    // Without a key nothing is sent yet: a correction still reopens the match.
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "create-1", 1).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 0, "game-1").await;
    assert_eq!(void_last(&f, &id, "void-1").await, StatusCode::CREATED);
    assert_eq!(match_state(&f, &id).await, "between_games");
    win(&f, &id, 1, "game-2").await;

    // Once the bridge starts sending the result, it stands, sent or not.
    let keyed = keyed_state(&f);
    f.blumint.lock().unwrap().fail = 1;
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("retrying".into(), 1)
    );
    assert_eq!(void_last(&f, &id, "void-2").await, StatusCode::CONFLICT);
    f.bridge.clock.advance(11);
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("delivered".into(), 2)
    );
    assert_eq!(void_last(&f, &id, "void-3").await, StatusCode::CONFLICT);
    assert_eq!(
        submitted(&f),
        vec![json!({ "mode": "result", "matchId": id, "teams": teams(&f, [0, 1]) })]
    );
}

#[tokio::test]
async fn cancelled_and_disputed_matches_are_restarted_on_blumint() {
    // BluMint's key is configured only after these matches end.
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "create-1", 2).await;
    let first = created["matchId"].as_str().unwrap().to_owned();
    // Kate won a game before it was called off; the abort keeps it.
    win(&f, &first, 0, "game-1").await;
    cancel(&f, &first, "cancel-1").await;

    // A match that would wait for an organizer's review has none at BluMint:
    // removing Sam's link cancels it here and frees its players.
    let (_, created) = create(&f, "create-2", 2).await;
    let second = created["matchId"].as_str().unwrap().to_owned();
    let sam = f.sam.id().to_string();
    let link: String = f
        .bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT id FROM links WHERE ember_id = ?1 AND connection_id = ?2 AND revoked_at IS NULL",
                [sam.as_str(), CONNECTION],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap();
    let removed = f
        .bridge
        .client
        .delete(f.bridge.url(&format!("/v1/links/{link}")))
        .bearer_auth(&f.provider)
        .send()
        .await
        .unwrap();
    assert_eq!(removed.status(), StatusCode::OK);
    assert_eq!(match_state(&f, &second).await, "cancelled");

    // Without the key nothing is sent; with it, both restarts are.
    ember_bridge::deliver_results(f.bridge.state()).await;
    assert!(submitted(&f).is_empty());
    let with_key = keyed_state(&f);
    ember_bridge::deliver_results(&with_key).await;
    let mut sent = submitted(&f);
    sent.sort_by_key(|body| body["matchId"].as_str().unwrap().to_owned());
    let mut restarts = vec![
        restart(&f, &first, "serverFailure", [1, 0]),
        restart(&f, &second, "insufficientPlayers", [0, 0]),
    ];
    restarts.sort_by_key(|body| body["matchId"].as_str().unwrap().to_owned());
    assert_eq!(sent, restarts);

    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (status, created) = create(&f, "create-3", 2).await;
    assert_eq!(
        status,
        StatusCode::OK,
        "the restarted match could not be created"
    );

    // Fighters whose reports disagree dispute the match, which is cancelled
    // for BluMint to ask again.
    let third = created["matchId"].as_str().unwrap().to_owned();
    let binding = bound(&f, &third).await;
    let game = permit(&f, &third, &binding, 1).await;
    report(&f, &f.kate, &f.kate, &third, &game, 1, Outcome::P1Win).await;
    report(&f, &f.sam, &f.sam, &third, &game, 2, Outcome::P2Win).await;
    assert_eq!(match_state(&f, &third).await, "cancelled");
    ember_bridge::deliver_results(&with_key).await;
    let sent = submitted(&f);
    assert_eq!(
        sent.last(),
        Some(&restart(&f, &third, "disputed", [0, 0])),
        "{sent:?}"
    );
}

// BluMint replays a game that was aborted, so the bridge does not wait a
// minute for the second fighter's abort: 20 seconds is enough, and the match
// is cancelled for BluMint to ask again. The clock follows the wall clock, so
// each check stays a few seconds clear of its deadline.
#[tokio::test]
async fn a_lone_abort_restarts_the_match_after_twenty_seconds() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    let binding = bound(&f, &id).await;
    let game = permit(&f, &id, &binding, 1).await;
    report(&f, &f.kate, &f.kate, &id, &game, 1, Outcome::Abort).await;
    f.bridge.clock.advance(15);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &id).await, "awaiting_reports");
    f.bridge.clock.advance(10);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &id).await, "cancelled");
    // The players are free for the set BluMint asks for again.
    let (status, _) = create(&f, "set-2", 2).await;
    assert_eq!(status, StatusCode::OK);
}

// A lone win can still be met by the other fighter's late report (a game
// retries its report for half a minute), so it keeps the full wait.
#[tokio::test]
async fn a_lone_win_still_waits_a_minute_on_blumint() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    let binding = bound(&f, &id).await;
    let game = permit(&f, &id, &binding, 1).await;
    report(&f, &f.kate, &f.kate, &id, &game, 1, Outcome::P1Win).await;
    f.bridge.clock.advance(25);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &id).await, "awaiting_reports");
    f.bridge.clock.advance(30);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &id).await, "awaiting_reports");
    f.bridge.clock.advance(10);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &id).await, "cancelled");
}

// A game called off before it started is closed the same way, and the set
// goes on to its next game.
#[tokio::test]
async fn a_lone_cancel_closes_the_game_after_twenty_seconds_on_blumint() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    let binding = bound(&f, &id).await;
    let game = permit(&f, &id, &binding, 1).await;
    report(&f, &f.kate, &f.kate, &id, &game, 1, Outcome::Cancel).await;
    f.bridge.clock.advance(15);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &id).await, "awaiting_reports");
    f.bridge.clock.advance(10);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &id).await, "between_games");
}

// Moving a Discord account to another Ember ID ends the links it made for
// the old one, and with them a set that ID is playing: the game in progress
// counts for nobody, and BluMint hears a retryable abort with the games
// already won, so it asks for the set again from that score.
#[tokio::test]
async fn moving_discord_mid_set_restarts_the_set_from_the_games_won() {
    let discord = support::fake_discord().await;
    let secrets = Secrets {
        discord_client_secret: Some(Zeroizing::new("test-secret".into())),
        ..keyed()
    };
    let f = fixture_adjusted(secrets, config::DEFAULT_MATCH_EXPIRY_HOURS, |config| {
        config.discord = Some(config::Discord {
            client_id: "1546980049692135514".into(),
            api_base: discord,
        });
        config.integration_secrets = Some("unused-in-tests.json".into());
    })
    .await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    let binding = bound(&f, &id).await;
    let first = permit(&f, &id, &binding, 1).await;
    report(&f, &f.kate, &f.kate, &id, &first, 1, Outcome::P1Win).await;
    report(&f, &f.sam, &f.sam, &id, &first, 2, Outcome::P1Win).await;
    permit(&f, &id, &binding, 2).await;

    // Kate moves her Discord account to a new PC's Ember ID during game 2.
    let new_pc = support::player(&f.bridge, 3).await;
    let state = support::start(&f.bridge, &new_pc).await;
    let page = support::back(&f.bridge, &format!("code=good-kate&state={state}")).await;
    assert!(page.contains("a game in progress does not count"), "{page}");
    let page = support::decide(&f.bridge, &support::confirmation(&page), "move").await;
    assert!(page.contains("Discord connected"), "{page}");

    assert_eq!(match_state(&f, &id).await, "cancelled");
    let aborts: Vec<Json> = reports(&f, &id)
        .await
        .into_iter()
        .filter(|body| body["mode"] == "abort")
        .collect();
    assert_eq!(
        aborts,
        vec![restart(&f, &id, "insufficientPlayers", [1, 0])]
    );
}

#[tokio::test]
async fn a_match_keeps_its_platforms_policy_after_the_connection_leaves_the_configuration() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "create-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    // A game was permitted and neither player reported it.
    seed::silent_permitted_attempt(f.bridge.state(), &id).await;
    // BluMint's connection leaves the configuration; then the game times out.
    let state = reconfigured(
        f.bridge.state(),
        |config| config.tenants.retain(|tenant| tenant.id != "bm"),
        Secrets::default(),
    );

    ember_bridge::sync_config(&state).await.unwrap();
    f.bridge.clock.advance(31 * 60);
    ember_bridge::maintain(&state).await;
    let match_id = id.clone();
    let (match_state, delivery): (String, String) = state
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT state, delivery_state FROM matches WHERE id = ?1",
                [&match_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?)
        })
        .await
        .unwrap();
    assert_eq!(
        (match_state.as_str(), delivery.as_str()),
        ("cancelled", "queued")
    );
}

#[tokio::test]
async fn a_disabled_connection_sends_nothing_until_enabled_again() {
    // The bridge's own worker has no key here, so only these passes send.
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "create-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    cancel(&f, &id, "cancel-1").await;
    // A bridge with the key, configured with the connection enabled or not.
    let keyed_state = |enabled: bool| {
        reconfigured(
            f.bridge.state(),
            |config| {
                for tenant in &mut config.tenants {
                    for connection in &mut tenant.connections {
                        if connection.id == CONNECTION {
                            connection.enabled = enabled;
                        }
                    }
                }
            },
            keyed(),
        )
    };

    let disabled = keyed_state(false);
    ember_bridge::sync_config(&disabled).await.unwrap();
    let credentials = || async {
        f.bridge
            .state()
            .db
            .read(|tx| {
                Ok(tx.query_row(
                    "SELECT COUNT(*) FROM service_credentials WHERE connection_id = ?1",
                    [CONNECTION],
                    |row| row.get::<_, i64>(0),
                )?)
            })
            .await
            .unwrap()
    };
    let issued = credentials().await;
    ember_bridge::deliver_results(&disabled).await;
    assert!(
        ember_bridge::register_blumint(&disabled, CONNECTION)
            .await
            .is_err()
    );
    // A bridge still configured with it enabled finds it disabled when leasing
    // and when registering.
    let stale = keyed_state(true);
    ember_bridge::deliver_results(&stale).await;
    assert!(
        ember_bridge::register_blumint(&stale, CONNECTION)
            .await
            .is_err()
    );
    assert!(f.blumint.lock().unwrap().calls.is_empty());
    assert_eq!(credentials().await, issued);
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("queued".into(), 0)
    );

    let enabled = keyed_state(true);
    ember_bridge::sync_config(&enabled).await.unwrap();
    ember_bridge::deliver_results(&enabled).await;
    assert_eq!(
        submitted(&f),
        vec![restart(&f, &id, "serverFailure", [0, 0])]
    );
}

// A match nobody played is sent as an abort BluMint must not retry: a new
// match would wait the same way, so BluMint's host takes the match over.
// Its players can be matched again, under a new key.
#[tokio::test]
async fn an_expired_match_is_sent_to_blumint_as_a_final_abort() {
    // An hour, so the match expires well inside the day its key is kept.
    let f = fixture_with(keyed(), 1).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "create-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("queued".into(), 0)
    );

    // The test clock follows the wall clock, so stay a few seconds clear of
    // the hour: a second that passes during the test must not end it early.
    f.bridge.clock.advance(60 * 60 - 10);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &id).await, "awaiting_players");
    // The same request while it is open still answers with the same match.
    let (_, again) = create(&f, "create-1", 2).await;
    assert_eq!(again["matchId"], id.as_str());

    f.bridge.clock.advance(10);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &id).await, "expired");
    results::until(&f.bridge, &id, |(state, _)| state == "delivered").await;
    assert_eq!(
        submitted(&f),
        vec![json!({
            "mode": "abort",
            "matchId": id,
            "reason": "insufficientPlayers",
            "retryable": false,
            "teams": teams(&f, [0, 0]),
        })]
    );

    // Its key still answers the match it made; BluMint asks for a new match
    // under a new key, which is a new match, not the expired one.
    let (status, again) = create(&f, "create-1", 2).await;
    assert_eq!(
        (status, again["matchId"].as_str()),
        (StatusCode::OK, Some(id.as_str()))
    );
    let (status, next) = create(&f, "create-2", 2).await;
    assert_eq!(status, StatusCode::OK, "{next}");
    assert_ne!(next["matchId"], id.as_str());
    let next = next["matchId"].as_str().unwrap().to_owned();
    assert_eq!(match_state(&f, &next).await, "awaiting_players");

    // A game played gives the match another lifetime from then, so a set
    // being played is not cut off between its games.
    f.bridge.clock.advance(30 * 60);
    win(&f, &next, 0, "game-1").await;
    f.bridge.clock.advance(30 * 60 + 10);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &next).await, "between_games");
    f.bridge.clock.advance(30 * 60);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &next).await, "expired");
}

// BluMint frees players from a match it could not close only for a day after
// the match was made, so a bridge whose quiet matches stay open longer still
// ends a BluMint match within that day. Its other connections keep the
// longer expiry.
#[tokio::test]
async fn a_blumint_match_lives_at_most_a_day_whatever_the_bridge_expiry() {
    const HOUR: u64 = 60 * 60;
    let f = fixture_with(Secrets::default(), 48).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "create-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    let (_, found) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    let lifetime = |found: &Json| {
        found["expires_at"].as_u64().unwrap() - found["created_at"].as_u64().unwrap()
    };
    assert_eq!(lifetime(&found), 24 * HOUR, "{found}");
    // A game late in the day moves the deadline no further than that.
    f.bridge.clock.advance(23 * 60 * 60);
    win(&f, &id, 0, "game-1").await;
    let (_, found) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(lifetime(&found), 24 * HOUR, "{found}");
    f.bridge.clock.advance(60 * 60 + 10);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &id).await, "expired");

    let (provider, direct) = direct_match(&f).await;
    let (_, found) = f
        .bridge
        .get(&provider, &format!("/v1/matches/{direct}"))
        .await;
    assert_eq!(lifetime(&found), 48 * HOUR, "{found}");
}

// A day after its creation a BluMint match ends even with a game open, since
// BluMint counts its players free from then: the game is called off, and the
// players can be matched again.
#[tokio::test]
async fn a_game_open_at_a_blumint_matchs_cap_does_not_hold_its_players() {
    let mut f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.advance(24 * 60 * 60 - 60);
    f.bridge.open_session(&mut f.kate).await;
    f.bridge.open_session(&mut f.sam).await;
    let binding = bound(&f, &id).await;
    permit(&f, &id, &binding, 1).await;
    f.bridge.clock.advance(80);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_state(&f, &id).await, "expired");
    let (_, found) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(found["attempts"][0]["state"], "aborted", "{found}");
    let (status, next) = create(&f, "set-2", 2).await;
    assert_eq!(status, StatusCode::OK, "{next}");
}

// BluMint shows a set's score as it is played, so each game won that does not
// end the set is reported; the game that ends it is the result, and a game
// that changes no score is not reported.
#[tokio::test]
async fn a_progress_report_follows_each_game_that_does_not_end_the_set() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    // BluMint already has the report: a 409 is as good as delivered.
    f.blumint.lock().unwrap().answers.push_back((409, None));
    win(&f, &id, 0, "game-1").await;
    assert_eq!(reports(&f, &id).await, vec![progressed(&f, &id, [1, 0])]);
    assert_eq!(progress(&f, &id).await, vec![(1, "delivered".into())]);
    draw(&f, &id, "game-2").await;
    win(&f, &id, 1, "game-3").await;
    assert_eq!(
        reports(&f, &id).await,
        vec![progressed(&f, &id, [1, 0]), progressed(&f, &id, [1, 1])]
    );
    win(&f, &id, 0, "game-4").await;
    assert_eq!(
        reports(&f, &id).await,
        vec![
            progressed(&f, &id, [1, 0]),
            progressed(&f, &id, [1, 1]),
            result(&f, &id, [2, 1]),
        ]
    );

    // A set called off at 1-0 is an abort that keeps the 1-0.
    let (_, created) = create(&f, "set-2", 2).await;
    let next = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &next, 0, "game-5").await;
    assert_eq!(
        reports(&f, &next).await,
        vec![progressed(&f, &next, [1, 0])]
    );
    cancel(&f, &next, "cancel-1").await;
    assert_eq!(
        reports(&f, &next).await,
        vec![
            progressed(&f, &next, [1, 0]),
            restart(&f, &next, "serverFailure", [1, 0]),
        ]
    );
}

// BluMint asks every report to name the creation that made its match by that
// creation's Idempotency-Key. The stand-in BluMint refuses a report without
// one, as BluMint does; each kind carries its own match's key. A set that ends
// drops a progress report still unsent, so game 1's is settled before game 2.
#[tokio::test]
async fn every_report_names_the_creation_that_made_its_match() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "tA1b2-round-1-match-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    assert!(reports(&f, &id).await.is_empty());
    assert!(!statuses(&f, &id).is_empty());
    win(&f, &id, 0, "game-1").await;
    assert_eq!(reports(&f, &id).await, vec![progressed(&f, &id, [1, 0])]);
    win(&f, &id, 0, "game-2").await;
    assert_eq!(
        reports(&f, &id).await,
        vec![progressed(&f, &id, [1, 0]), result(&f, &id, [2, 0])]
    );

    let (_, created) = create(&f, "tA1b2-round-1-match-2", 2).await;
    let next = created["matchId"].as_str().unwrap().to_owned();
    cancel(&f, &next, "cancel-1").await;
    assert_eq!(
        reports(&f, &next).await,
        vec![restart(&f, &next, "serverFailure", [0, 0])]
    );

    for (id, key, modes) in [
        (
            &id,
            "tA1b2-round-1-match-1",
            &["status", "progress", "result"][..],
        ),
        (&next, "tA1b2-round-1-match-2", &["abort"][..]),
    ] {
        let sent = report_keys(&f, id);
        assert!(sent.iter().all(|(_, sent)| sent == key), "{sent:?}");
        for mode in modes {
            assert!(
                sent.iter().any(|(sent, _)| sent == mode),
                "{mode}: {sent:?}"
            );
        }
    }
}

// BluMint keeps the wins a progress report gave a team and refuses any later
// report below them, the result included. So once one has been sent, a game
// it may count cannot be voided, mid-set or in a correction.
#[tokio::test]
async fn a_game_blumint_has_been_told_of_cannot_be_voided() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 0, "game-1").await;
    assert_eq!(reports(&f, &id).await, vec![progressed(&f, &id, [1, 0])]);
    assert_eq!(void_last(&f, &id, "void-1").await, StatusCode::CONFLICT);
    assert_eq!(match_state(&f, &id).await, "between_games");
    win(&f, &id, 1, "game-2").await;
    assert_eq!(
        reports(&f, &id).await,
        vec![progressed(&f, &id, [1, 0]), progressed(&f, &id, [1, 1])]
    );
    win(&f, &id, 1, "game-3").await;
    assert_eq!(
        reports(&f, &id).await,
        vec![
            progressed(&f, &id, [1, 0]),
            progressed(&f, &id, [1, 1]),
            result(&f, &id, [1, 2]),
        ]
    );

    // A completed set whose result is still to be sent, with 1-0 and 1-1
    // sent: voiding Sam's game would send 2-0 over the 1 BluMint gave him.
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 0, "game-1").await;
    ember_bridge::deliver_results(&keyed).await;
    win(&f, &id, 1, "game-2").await;
    ember_bridge::deliver_results(&keyed).await;
    win(&f, &id, 0, "game-3").await;
    assert_eq!(match_state(&f, &id).await, "completed");
    assert_eq!(void_game(&f, &id, 1, "void-1").await, StatusCode::CONFLICT);
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(
        submitted(&f),
        vec![
            progressed(&f, &id, [1, 0]),
            progressed(&f, &id, [1, 1]),
            result(&f, &id, [2, 1]),
        ]
    );
}

// A game voided before any report on it was sent can still be voided, and
// its report goes out with the score as it then stands. The next game won
// brings that report's count back, so it is sent again with the new score.
#[tokio::test]
async fn a_report_whose_count_comes_back_after_a_void_is_sent_again() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 3).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 0, "game-1").await;
    assert_eq!(void_last(&f, &id, "void-1").await, StatusCode::CREATED);
    ember_bridge::deliver_results(&keyed).await;
    win(&f, &id, 1, "game-2").await;
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(
        submitted(&f),
        vec![progressed(&f, &id, [0, 0]), progressed(&f, &id, [0, 1])]
    );
    assert_eq!(progress(&f, &id).await, vec![(1, "delivered".into())]);
}

// BluMint keeps no score for a match it does not know, so a progress report
// it answers 404 is not sent again; only the status report is.
#[tokio::test]
async fn a_progress_report_on_a_match_blumint_does_not_know_is_not_sent_again() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.blumint.lock().unwrap().answers.push_back((404, None));
    win(&f, &id, 0, "game-1").await;
    assert_eq!(reports(&f, &id).await, vec![progressed(&f, &id, [1, 0])]);
    assert_eq!(progress(&f, &id).await, vec![(1, "failed".into())]);
    f.bridge.clock.advance(2);
    assert_eq!(reports(&f, &id).await, vec![progressed(&f, &id, [1, 0])]);
}

// A report still waiting when the set ends is never sent: it would tell
// BluMint a finished set is still on, and the result has the final score.
#[tokio::test]
async fn a_report_on_a_set_that_has_ended_is_not_sent() {
    // Without a key nothing is sent until the set is over.
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 0, "game-1").await;
    win(&f, &id, 0, "game-2").await;
    assert_eq!(progress(&f, &id).await.len(), 1);
    let keyed = keyed_state(&f);
    ember_bridge::deliver_results(&keyed).await;
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(submitted(&f), vec![result(&f, &id, [2, 0])]);
    assert_eq!(progress(&f, &id).await, vec![(1, "not_required".into())]);
}
