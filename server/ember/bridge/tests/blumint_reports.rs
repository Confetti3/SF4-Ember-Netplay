//! What the bridge reports to a stand-in BluMint: a match's status as its
//! players arrive and its games start and end, its score after each game,
//! its result, and an abort for a match that ended without one, with retries
//! and the starting wins of a set asked for again.
mod common;
#[path = "discord/support.rs"]
#[allow(dead_code)]
mod support;

use common::{
    Player,
    blumint::*,
    play::{BUILD, bound, claim, claim_ready, endpoint, permit, report},
    results,
};
use ember_bridge::{AppState, Keys, config, integrations::Secrets};
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
    let match_id = id.clone();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO attempts (id, match_id, assignment_generation, seq, source, state, created_at, start_by, permit_id)
                 VALUES ('att_silent', ?1, 1, 1, 'player_agreement', 'permitted', 0, 0, 'pmt_silent')",
                [&match_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    // BluMint's connection leaves the configuration; then the game times out.
    let mut config = (*f.bridge.state().config).clone();
    config.tenants.retain(|tenant| tenant.id != "bm");
    let state = AppState::new(
        config,
        Keys::generate(),
        Secrets::default(),
        f.bridge.state().db.clone(),
        f.bridge.clock.clone(),
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
        let state = f.bridge.state();
        let mut config = (*state.config).clone();
        for tenant in &mut config.tenants {
            for connection in &mut tenant.connections {
                if connection.id == CONNECTION {
                    connection.enabled = enabled;
                }
            }
        }
        AppState::new(
            config,
            Keys::generate(),
            keyed(),
            state.db.clone(),
            f.bridge.clock.clone(),
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

const ABSENT: &str = "absent";
const NOT_READY: &str = "present-not-ready";
const READY: &str = "present-ready";

/// The status reports sent for the match so far, without the times they
/// were observed, which never go back.
fn sent_statuses(f: &Fixture, id: &str) -> Vec<Json> {
    sent_statuses_at(f, id).0
}

/// The status reports sent for the match so far, and apart from them the
/// times they were observed, which never go back.
fn sent_statuses_at(f: &Fixture, id: &str) -> (Vec<Json>, Vec<String>) {
    let (sent, times): (Vec<Json>, Vec<String>) = statuses(f, id).into_iter().map(untimed).unzip();
    for at in &times {
        assert!(ember_protocol::event::is_rfc3339_utc(at), "{at}");
    }
    assert!(times.windows(2).all(|pair| pair[0] <= pair[1]), "{times:?}");
    (sent, times)
}

// BluMint reads who has turned up from the status, so the bridge sends it on
// every change: as each player arrives, as each game starts, and as each game
// ends with another to come, when both are waiting for the next. Each is the
// match's next revision. It is pending until the fighters' first game starts.
// A game an agreed abort ends, and the set's end, change it no further.
#[tokio::test]
async fn a_status_follows_each_change_of_presence() {
    // Without a key only the test's own passes send, so each change is sent
    // before the next.
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    ember_bridge::deliver_results(&keyed).await;
    assert!(statuses(&f, &id).is_empty());

    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // Sam arrives while Kate is setting up the room, which moves nothing
    // else about the match.
    let (status_code, wait) = claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    assert_eq!(
        (status_code, wait["role"].as_str()),
        (StatusCode::OK, Some("wait"))
    );
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(statuses(&f, &id).len(), 2);
    // Both claim again as the room is set up, which changes nothing.
    let binding = bound(&f, &id).await;
    ember_bridge::deliver_results(&keyed).await;
    let first = permit(&f, &id, &binding, 1).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.kate, &f.kate, &id, &first, 1, Outcome::Abort).await;
    report(&f, &f.sam, &f.sam, &id, &first, 2, Outcome::Abort).await;
    ember_bridge::deliver_results(&keyed).await;
    let second = permit(&f, &id, &binding, 2).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.kate, &f.kate, &id, &second, 3, Outcome::P1Win).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.sam, &f.sam, &id, &second, 4, Outcome::P1Win).await;
    ember_bridge::deliver_results(&keyed).await;
    let third = permit(&f, &id, &binding, 3).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.kate, &f.kate, &id, &third, 5, Outcome::P1Win).await;
    let (_, decided) = report(&f, &f.sam, &f.sam, &id, &third, 6, Outcome::P1Win).await;
    assert_eq!(decided["attempt_state"], "accepted", "{decided}");
    ember_bridge::deliver_results(&keyed).await;

    assert_eq!(
        sent_statuses(&f, &id),
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "running", [READY, READY]),
            status(&f, &id, 4, "running", [NOT_READY, NOT_READY]),
            status(&f, &id, 5, "running", [READY, READY]),
            status(&f, &id, 6, "running", [NOT_READY, NOT_READY]),
            status(&f, &id, 7, "running", [READY, READY]),
        ]
    );
    assert_eq!(
        submitted(&f),
        vec![progressed(&f, &id, [1, 0]), result(&f, &id, [2, 0])]
    );
    // The status has a row of its own, apart from the progress reports.
    assert_eq!(progress(&f, &id).await, vec![(1, "delivered".into())]);
    assert_eq!(status_row(&f, &id).await, Some((7, "delivered".into())));
}

// Each fighter's Ember says with every claim whether they pressed Ready at
// the match's table, and claims again at once when that changes, so BluMint
// can tell the player who stalls from the one waiting on them. Each change
// is the status's next revision; a claim that changes nothing sends nothing.
// A decided game takes back both fighters' Ready as it settles, since the room
// clears both seats when a game is played, before each Ember's claim says so.
// A game called off because one fighter took Ready back leaves the other
// Ready, as their Ember last said, and they read so. An Ember too old to say
// reads not ready until a game starts.
#[tokio::test]
async fn a_player_reads_ready_once_their_ember_says_they_pressed_ready() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    // Both open the match on an Ember too old to say: the first status
    // BluMint is sent has both present, not ready.
    let binding = bound(&f, &id).await;
    ember_bridge::deliver_results(&keyed).await;
    for (ready, who) in [
        (true, &f.kate),
        (true, &f.kate),
        // Kate takes Ready back, or leaves her seat.
        (false, &f.kate),
        (true, &f.sam),
        (true, &f.kate),
    ] {
        let (status_code, answer) = claim_ready(&f, who, &id, ready).await;
        assert_eq!(status_code, StatusCode::OK, "{answer}");
        ember_bridge::deliver_results(&keyed).await;
    }
    let first = permit(&f, &id, &binding, 1).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.kate, &f.kate, &id, &first, 1, Outcome::P1Win).await;
    report(&f, &f.sam, &f.sam, &id, &first, 2, Outcome::P1Win).await;
    ember_bridge::deliver_results(&keyed).await;
    // The game's end clears both seats at the table, and each Ember says so.
    // An older Ember's claim between games still says nothing.
    for who in [&f.kate, &f.sam] {
        claim_ready(&f, who, &id, false).await;
        ember_bridge::deliver_results(&keyed).await;
    }
    claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // Both press Ready, and Sam takes it back once the game is permitted,
    // which calls it off: both report it cancelled. Kate is still Ready.
    for who in [&f.kate, &f.sam] {
        claim_ready(&f, who, &id, true).await;
        ember_bridge::deliver_results(&keyed).await;
    }
    let second = permit(&f, &id, &binding, 2).await;
    claim_ready(&f, &f.sam, &id, false).await;
    ember_bridge::deliver_results(&keyed).await;
    report(&f, &f.kate, &f.kate, &id, &second, 3, Outcome::Cancel).await;
    report(&f, &f.sam, &f.sam, &id, &second, 4, Outcome::Cancel).await;
    ember_bridge::deliver_results(&keyed).await;
    let (_, decided) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(decided["state"], "between_games", "{decided}");

    assert_eq!(
        sent_statuses(&f, &id),
        vec![
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [READY, NOT_READY]),
            status(&f, &id, 4, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 5, "pending", [NOT_READY, READY]),
            status(&f, &id, 6, "pending", [READY, READY]),
            status(&f, &id, 7, "running", [READY, READY]),
            status(&f, &id, 8, "running", [NOT_READY, NOT_READY]),
            status(&f, &id, 9, "running", [READY, NOT_READY]),
            status(&f, &id, 10, "running", [READY, READY]),
            status(&f, &id, 11, "running", [READY, NOT_READY]),
        ]
    );
}

// A fighter who presses Ready for the next game before this one is decided
// stays Ready once it is, while the other's Ready from before the game ended
// is taken back without waiting for their Ember's claim.
#[tokio::test]
async fn a_ready_pressed_after_a_game_ended_outlasts_its_result() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let binding = bound(&f, &id).await;
    for who in [&f.kate, &f.sam] {
        claim_ready(&f, who, &id, true).await;
    }
    let first = permit(&f, &id, &binding, 1).await;
    f.bridge.clock.advance(1);
    report(&f, &f.kate, &f.kate, &id, &first, 1, Outcome::P1Win).await;
    f.bridge.clock.advance(1);
    claim_ready(&f, &f.kate, &id, true).await;
    report(&f, &f.sam, &f.sam, &id, &first, 2, Outcome::P1Win).await;
    ember_bridge::deliver_results(&keyed).await;
    let sent = sent_statuses(&f, &id);
    let last = sent.last().unwrap();
    assert_eq!(
        last["players"]
            .as_array()
            .unwrap()
            .iter()
            .map(|player| player["status"].clone())
            .collect::<Vec<_>>(),
        vec![json!(READY), json!(NOT_READY)],
        "{sent:?}"
    );
}

// A status BluMint has not taken is sent again as it stands, the same
// revision observed at the same time; BluMint asks for that while it does not
// know the match yet. One that changes before then is sent as it now
// stands, under its next revision, and the earlier one is not sent again.
#[tokio::test]
async fn a_status_is_sent_again_until_a_later_one_replaces_it() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let start = f.bridge.clock.now();

    f.blumint.lock().unwrap().answers.push_back((404, None));
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(status_row(&f, &id).await, Some((1, "retrying".into())));
    f.bridge.clock.advance(2);
    ember_bridge::deliver_results(&keyed).await;
    let sent = statuses(&f, &id);
    assert_eq!(sent.len(), 2);
    assert_eq!(sent[0], sent[1]);
    assert_eq!(status_row(&f, &id).await, Some((1, "delivered".into())));

    // Sam's arrival waits on BluMint, and the game starts meanwhile.
    // A wait shorter than the claim lease, so both fighters stay present.
    f.blumint.lock().unwrap().answers.push_back((503, Some(5)));
    let binding = bound(&f, &id).await;
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(status_row(&f, &id).await, Some((2, "retrying".into())));
    permit(&f, &id, &binding, 1).await;
    ember_bridge::deliver_results(&keyed).await;
    f.bridge.clock.advance(6);
    ember_bridge::deliver_results(&keyed).await;
    let (sent, times) = sent_statuses_at(&f, &id);
    assert_eq!(
        sent,
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "running", [READY, READY]),
        ]
    );
    // Each revision carries when it was observed, not when the match's
    // first one was.
    let observed = [start, start, start + 2, start + 2].map(ember_protocol::event::rfc3339);
    assert_eq!(times, observed);
    assert_eq!(status_row(&f, &id).await, Some((3, "delivered".into())));
}

/// The presence lease, as the clock's signed seconds.
const LEASE: i64 = ember_bridge::PRESENCE_LEASE_SECS as i64;

/// Advances the clock `secs` in steps of at most 20 seconds, `who` claiming,
/// the maintenance pass running and BluMint being sent what it is owed after
/// each.
async fn claiming_for(f: &Fixture, keyed: &AppState, who: &Player, id: &str, secs: i64) {
    let mut left = secs;
    while left > 0 {
        let step = left.min(20);
        f.bridge.clock.advance(step);
        claim(f, who, id, &endpoint(who), BUILD).await;
        ember_bridge::maintain(f.bridge.state()).await;
        ember_bridge::deliver_results(keyed).await;
        left -= step;
    }
}

/// Runs the maintenance pass and sends BluMint what it is owed.
async fn sweep(f: &Fixture, keyed: &AppState) {
    ember_bridge::maintain(f.bridge.state()).await;
    ember_bridge::deliver_results(keyed).await;
}

// A player is present only while their Ember is connected to the match. It
// claims the match again ten seconds after each answer while open, so a
// claim older than the lease means it left: the maintenance pass sees the
// claim lapse and BluMint hears the player is absent, whether or not the
// other one is still there. A late claim, as one the helper gave up on and
// retried is, keeps them present, whoever observes the match meanwhile.
// Claiming again brings them back.
#[tokio::test]
async fn a_player_whose_ember_stops_claiming_reads_as_absent() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let start = f.bridge.clock.now();
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;

    // Kate keeps claiming; Sam stops, and his claim lapses after its lease.
    claiming_for(&f, &keyed, &f.kate, &id, LEASE - 1).await;
    assert_eq!(statuses(&f, &id).len(), 2);
    f.bridge.clock.advance(1);
    sweep(&f, &keyed).await;
    // Sam comes back.
    f.bridge.clock.advance(5);
    claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // Then both leave.
    f.bridge.clock.advance(LEASE + 1);
    ember_bridge::maintain(f.bridge.state()).await;
    ember_bridge::deliver_results(&keyed).await;
    ember_bridge::maintain(f.bridge.state()).await;
    ember_bridge::deliver_results(&keyed).await;

    let (sent, times) = sent_statuses_at(&f, &id);
    assert_eq!(
        sent,
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 4, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 5, "pending", [ABSENT, ABSENT]),
        ]
    );
    let lease = ember_bridge::PRESENCE_LEASE_SECS;
    let observed = [
        start,
        start,
        start + lease,
        start + lease + 5,
        start + 2 * lease + 6,
    ];
    assert_eq!(times, observed.map(ember_protocol::event::rfc3339));
}

// The bridge hears no claims while it is down, so a claim live when it
// stopped is not counted lapsed for that time: once it starts again, each
// fighter has a lease to claim again in. A claim that lapsed before it
// stopped stays lapsed.
#[tokio::test]
async fn a_restart_does_not_read_a_connected_player_as_absent() {
    let mut f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // Sam leaves; Kate stays.
    claiming_for(&f, &keyed, &f.kate, &id, LEASE).await;
    f.bridge.clock.advance(1);
    sweep(&f, &keyed).await;
    assert_eq!(statuses(&f, &id).len(), 3);

    // The bridge is down for longer than a lease, and sweeps as it starts.
    f.bridge.clock.advance(LEASE + 30);
    f.bridge.restart(Secrets::default()).await;
    let keyed = keyed_state(&f);
    ember_bridge::maintain(f.bridge.state()).await;
    ember_bridge::deliver_results(&keyed).await;
    f.bridge.clock.advance(5);
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;

    assert_eq!(
        sent_statuses(&f, &id),
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [NOT_READY, ABSENT]),
        ]
    );
}

// A bridge that stops again within a lease of starting keeps the first
// stop's span: a claim live through both stops gets a lease after the second,
// then lapses and is swept, so BluMint hears both players left.
#[tokio::test]
async fn a_second_restart_within_a_lease_still_sees_both_players_leave() {
    let mut f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    sweep(&f, &keyed).await;
    f.bridge.clock.advance(5);
    for who in [&f.kate, &f.sam] {
        claim(&f, who, &id, &endpoint(who), BUILD).await;
        ember_bridge::deliver_results(&keyed).await;
    }
    // Both quit while the bridge is down. It starts again, sweeps once, and
    // stops again before the first start's lease ends.
    f.bridge.clock.advance(LEASE + 55);
    f.bridge.restart(Secrets::default()).await;
    f.bridge.clock.advance(30);
    sweep(&f, &keyed_state(&f)).await;
    f.bridge.clock.advance(20);
    f.bridge.restart(Secrets::default()).await;
    let keyed = keyed_state(&f);
    for _ in 0..(LEASE / 10 + 2) {
        f.bridge.clock.advance(10);
        sweep(&f, &keyed).await;
    }

    assert_eq!(
        sent_statuses(&f, &id),
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [ABSENT, ABSENT]),
        ]
    );
}

// A claim can lapse after the bridge's last sweep and before it stops, and
// the other fighter's claim then tells BluMint that player left. The bridge
// was running then, so that claim is not extended across the stop, and
// BluMint does not hear the player come back.
#[tokio::test]
async fn a_claim_that_lapsed_before_a_stop_stays_lapsed_after_it() {
    let mut f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    for who in [&f.kate, &f.sam] {
        claim(&f, who, &id, &endpoint(who), BUILD).await;
        ember_bridge::deliver_results(&keyed).await;
    }
    // Sam stops claiming. The last sweep runs before his claim lapses, and
    // Kate's next claim, after it lapses, observes him absent.
    let mut waited = 0;
    while waited + 20 < LEASE - 5 {
        f.bridge.clock.advance(20);
        waited += 20;
        claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    }
    f.bridge.clock.advance(LEASE - 5 - waited);
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    sweep(&f, &keyed).await;
    f.bridge.clock.advance(6);
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // The bridge stops a few seconds later, and starts again within Kate's
    // lease; she claims again.
    f.bridge.clock.advance(29);
    f.bridge.restart(Secrets::default()).await;
    let keyed = keyed_state(&f);
    f.bridge.clock.advance(5);
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    f.bridge.clock.advance(5);
    sweep(&f, &keyed).await;

    assert_eq!(
        sent_statuses(&f, &id),
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [NOT_READY, ABSENT]),
        ]
    );
}

// A wall clock stepped forward and then back leaves a status observed ahead
// of the time. It is not followed: both fighters, claiming as usual, read
// present again at once rather than once the clock catches up.
#[tokio::test]
async fn a_clock_stepped_back_does_not_read_connected_players_as_absent() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let start = f.bridge.clock.now();
    for who in [&f.kate, &f.sam] {
        claim(&f, who, &id, &endpoint(who), BUILD).await;
        ember_bridge::deliver_results(&keyed).await;
    }
    f.bridge.clock.advance(3600);
    sweep(&f, &keyed).await;
    f.bridge.clock.advance(-3590);
    for who in [&f.kate, &f.sam] {
        claim(&f, who, &id, &endpoint(who), BUILD).await;
        ember_bridge::deliver_results(&keyed).await;
    }

    // The times observed follow the clock back, unlike any other report's.
    let (sent, times): (Vec<Json>, Vec<String>) =
        statuses(&f, &id).into_iter().map(untimed).unzip();
    assert_eq!(
        sent,
        vec![
            status(&f, &id, 1, "pending", [NOT_READY, ABSENT]),
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [ABSENT, ABSENT]),
            status(&f, &id, 4, "pending", [NOT_READY, NOT_READY]),
        ]
    );
    assert_eq!(times[3], ember_protocol::event::rfc3339(start + 10));
}

// Every observer reads the clock before it waits for the database, so a
// claim can commit after a maintenance pass that read a later time. The
// status it observes is the pass's: a fighter the pass saw lapse stays
// absent, and the time observed never goes back.
#[tokio::test]
async fn a_late_commit_does_not_bring_back_a_lapsed_player() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let start = f.bridge.clock.now();
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;
    // The pass that sees Sam lapse runs right after Kate's claim.
    claiming_for(&f, &keyed, &f.kate, &id, LEASE).await;
    // Kate's next claim read the clock a second before the pass did.
    f.bridge.clock.advance(-1);
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    ember_bridge::deliver_results(&keyed).await;

    let (sent, times) = sent_statuses_at(&f, &id);
    assert_eq!(
        sent,
        vec![
            status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
            status(&f, &id, 3, "pending", [NOT_READY, ABSENT]),
        ]
    );
    assert_eq!(
        times,
        [start, start + ember_bridge::PRESENCE_LEASE_SECS].map(ember_protocol::event::rfc3339)
    );
}

// A fighter whose build differs from the one the other fighter claimed with
// cannot play the match on it, but they did open it: BluMint hears they are
// present, not that they never came, and the bridge cannot tell whether they
// stay, since their Ember stops claiming. Claiming again on the same build as
// the other fighter changes nothing BluMint hears, and from then on they are
// present while they keep claiming, like the other.
#[tokio::test]
async fn a_player_refused_for_their_build_reads_as_present() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.bridge.clock.freeze();
    let start = f.bridge.clock.now();
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    let (status_code, refused) = claim(&f, &f.sam, &id, &endpoint(&f.sam), "build-2").await;
    assert_eq!(
        (status_code, refused["error"]["code"].as_str()),
        (StatusCode::UNPROCESSABLE_ENTITY, Some("incompatible_build")),
        "{refused}"
    );
    ember_bridge::deliver_results(&keyed).await;
    let mut expected = vec![status(&f, &id, 2, "pending", [NOT_READY, NOT_READY])];
    assert_eq!(sent_statuses(&f, &id), expected);
    claiming_for(&f, &keyed, &f.kate, &id, LEASE).await;
    f.bridge.clock.advance(1);
    sweep(&f, &keyed).await;
    assert_eq!(sent_statuses(&f, &id), expected);

    let (status_code, _) = claim(&f, &f.sam, &id, &endpoint(&f.sam), BUILD).await;
    assert_eq!(status_code, StatusCode::OK);
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(sent_statuses(&f, &id), expected);
    // Sam then leaves.
    claiming_for(&f, &keyed, &f.kate, &id, LEASE).await;
    expected.push(status(&f, &id, 3, "pending", [NOT_READY, ABSENT]));
    let (sent, times) = sent_statuses_at(&f, &id);
    assert_eq!(sent, expected);
    assert_eq!(
        times[1],
        ember_protocol::event::rfc3339(start + 2 * ember_bridge::PRESENCE_LEASE_SECS + 1)
    );
}

// A status still waiting when the match ends is never sent, and the match's
// end sends none: the result or abort says how it ended.
#[tokio::test]
async fn no_status_is_sent_once_the_match_has_ended() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    claim(&f, &f.kate, &id, &endpoint(&f.kate), BUILD).await;
    cancel(&f, &id, "cancel-1").await;
    ember_bridge::deliver_results(&keyed).await;
    ember_bridge::deliver_results(&keyed).await;
    assert!(statuses(&f, &id).is_empty());
    assert_eq!(
        submitted(&f),
        vec![restart(&f, &id, "serverFailure", [0, 0])]
    );
    assert_eq!(status_row(&f, &id).await, Some((1, "not_required".into())));
}

// A game an organizer recorded before the fighters played is not their first
// game: it says nothing of who is present, and the status stays pending until
// the fighters' first game starts.
#[tokio::test]
async fn an_organizers_game_leaves_the_status_pending() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let keyed = keyed_state(&f);
    for (key, game) in [("set-1", "game-1"), ("set-2", "game-2")] {
        let (_, created) = create(&f, key, 2).await;
        let id = created["matchId"].as_str().unwrap().to_owned();
        if key == "set-1" {
            draw(&f, &id, game).await;
        } else {
            win(&f, &id, 0, game).await;
        }
        ember_bridge::deliver_results(&keyed).await;
        assert_eq!(status_row(&f, &id).await, None);
        // Both arrive before the next pass, which sends the second arrival's
        // status alone.
        let binding = bound(&f, &id).await;
        ember_bridge::deliver_results(&keyed).await;
        permit(&f, &id, &binding, 1).await;
        ember_bridge::deliver_results(&keyed).await;
        assert_eq!(
            sent_statuses(&f, &id),
            vec![
                status(&f, &id, 2, "pending", [NOT_READY, NOT_READY]),
                status(&f, &id, 3, "running", [READY, READY]),
            ],
            "{key}"
        );
        close(&f, json!({ "idempotencyKey": key, "reason": "removed" })).await;
    }
}

// How the bridge reads each of BluMint's answers to a report: BluMint
// already having it is as good as delivered, a match it is not ready for
// waits as long as it asks, its own failure is tried again, and a match it
// does not know is not.
#[tokio::test]
async fn a_report_is_settled_as_blumints_answer_to_it_says() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let mut set = 0;
    let mut finished = |answers: Vec<(u16, Option<u64>)>| {
        set += 1;
        let key = format!("set-{set}");
        let f = &f;
        async move {
            let (_, created) = create(f, &key, 1).await;
            let id = created["matchId"].as_str().unwrap().to_owned();
            f.blumint.lock().unwrap().answers.extend(answers);
            win(f, &id, 0, &format!("{key}-game")).await;
            id
        }
    };

    let id = finished(vec![(409, None)]).await;
    results::until(&f.bridge, &id, |(state, _)| state == "delivered").await;

    let id = finished(vec![(503, Some(30)), (500, None)]).await;
    results::until(&f.bridge, &id, |(state, attempts)| {
        state == "retrying" && attempts == 1
    })
    .await;
    assert!(next_attempt(&f, &id).await >= f.bridge.clock.now() + 29);
    f.bridge.clock.advance(31);
    results::until(&f.bridge, &id, |(state, attempts)| {
        state == "retrying" && attempts == 2
    })
    .await;
    f.bridge.clock.advance(7);
    results::until(&f.bridge, &id, |(state, _)| state == "delivered").await;

    for refused in [404, 400] {
        let id = finished(vec![(refused, None)]).await;
        results::until(&f.bridge, &id, |(state, _)| state == "failed").await;
        assert_eq!(results::delivery(&f.bridge, &id).await.1, 1, "{refused}");
    }
    // A key BluMint does not know, or one bound to no game, waits for its
    // operator to fix it.
    for unauthorized in [401, 403] {
        let id = finished(vec![(unauthorized, None)]).await;
        results::until(&f.bridge, &id, |(state, attempts)| {
            state == "retrying" && attempts == 1
        })
        .await;
        f.bridge.clock.advance(2);
        results::until(&f.bridge, &id, |(state, _)| state == "delivered").await;
    }
}

// BluMint asks for a set again after its match ended without a result, with
// the games each team had won. The set carries on from that score, and every
// report counts it, so the set ends when a team reaches its length in all.
#[tokio::test]
async fn starting_wins_continue_the_set() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (status, created) = create_with(&f, "set-1", carried(&f, 2, [1, 0])).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 1, "game-1").await;
    assert_eq!(reports(&f, &id).await, vec![progressed(&f, &id, [1, 1])]);
    win(&f, &id, 0, "game-2").await;
    assert_eq!(
        reports(&f, &id).await,
        vec![progressed(&f, &id, [1, 1]), result(&f, &id, [2, 1])]
    );
    let completed: Vec<Json> = f
        .bridge
        .events(&f.provider, "0")
        .await
        .into_iter()
        .filter(|event| {
            event["type"] == "io.ember.tournament.match.completed.v1"
                && event["data"]["match_id"] == id
        })
        .collect();
    assert_eq!(completed[0]["data"]["starting_wins"], json!([1, 0]));

    let (_, created) = create_with(&f, "set-2", carried(&f, 2, [1, 0])).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 0, "game-3").await;
    assert_eq!(reports(&f, &id).await, vec![result(&f, &id, [2, 0])]);

    // Each player's record counts both sets from their starting score, and
    // only the games played in them.
    let (status, kate) = record(&f, &f.kate).await;
    assert_eq!(status, StatusCode::OK, "{kate}");
    assert_eq!(kate["sets"], json!({ "played": 2, "won": 2, "lost": 0 }));
    assert_eq!(kate["games"], json!({ "won": 2, "lost": 1, "drawn": 0 }));
    assert_eq!(
        (
            &kate["recent"][0]["result"],
            &kate["recent"][0]["wins"],
            &kate["recent"][0]["opponent_wins"]
        ),
        (&json!("won"), &json!(2), &json!(0))
    );
    let (_, sam) = record(&f, &f.sam).await;
    assert_eq!(sam["sets"], json!({ "played": 2, "won": 0, "lost": 2 }));
    assert_eq!(
        (
            &sam["recent"][0]["result"],
            &sam["recent"][0]["wins"],
            &sam["recent"][0]["opponent_wins"]
        ),
        (&json!("lost"), &json!(0), &json!(2))
    );

    // Both teams or neither, each a whole number short of the set.
    let mut one_sided = request(&f, 2);
    one_sided["teams"][0]["startingWins"] = json!(1);
    for body in [carried(&f, 2, [2, 0]), carried(&f, 2, [0, 3]), one_sided, {
        let mut negative = carried(&f, 2, [0, 0]);
        negative["teams"][1]["startingWins"] = json!(-1);
        negative
    }] {
        let (status, refused) = create_with(&f, "set-3", body.clone()).await;
        assert_eq!(
            (status, refused),
            (
                StatusCode::BAD_REQUEST,
                json!({ "code": "invalid_request" })
            ),
            "{body}"
        );
    }

    // After a reset BluMint sends 0 for every team, which is a set from the
    // start: the same match as one with no starting wins.
    let (status, created) = create_with(&f, "set-3", carried(&f, 2, [0, 0])).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    assert_eq!(
        create(&f, "set-3", 2).await,
        (StatusCode::OK, created.clone())
    );
    assert_eq!(
        create_with(&f, "set-3", carried(&f, 2, [1, 0])).await,
        (StatusCode::CONFLICT, json!({ "code": "key_reused" }))
    );
    let id = created["matchId"].as_str().unwrap().to_owned();
    let binding = bound(&f, &id).await;
    assert_eq!(binding["version"], 1);
    assert!(binding.get("starting_wins").is_none(), "{binding}");
    close(
        &f,
        json!({ "idempotencyKey": "set-3", "reason": "removed" }),
    )
    .await;

    // The room the fighters play in starts from the score too.
    let (_, created) = create_with(&f, "set-4", carried(&f, 2, [1, 0])).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    let binding = bound(&f, &id).await;
    assert_eq!(binding["version"], 1);
    assert_eq!(binding["starting_wins"], json!([1, 0]));
    let game = permit(&f, &id, &binding, 1).await;
    report(&f, &f.kate, &f.kate, &id, &game, 1, Outcome::P1Win).await;
    report(&f, &f.sam, &f.sam, &id, &game, 2, Outcome::P1Win).await;
    assert_eq!(match_state(&f, &id).await, "completed");
}

async fn record(f: &Fixture, player: &Player) -> (StatusCode, Json) {
    f.bridge
        .get(&f.provider, &format!("/v1/players/{}/record", player.id()))
        .await
}
