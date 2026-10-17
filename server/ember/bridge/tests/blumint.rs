//! BluMint's partner API against a stand-in BluMint: player lookup by Discord
//! account, match creation with a Play link and its refusals, closes, and
//! registration of the three endpoints. tests/blumint_reports.rs covers what
//! the bridge reports to BluMint.
mod common;

use common::{
    Player,
    blumint::*,
    play::{BUILD, claim, endpoint},
    results,
};
use ember_bridge::{config, integrations::Secrets};
use reqwest::StatusCode;
use serde_json::json;

#[tokio::test]
async fn lookup_finds_players_by_their_discord_accounts() {
    let f = fixture().await;
    // Every sign-in method arrives; only Discord is answered, emails skipped.
    let found = lookup(
        &f,
        json!({ "email": ["kate@example.com"], "steam": ["76561198833313974"], "discord": [KATE, "kate@example.com"] }),
    )
    .await;
    assert_eq!(found, json!({ "discord": [f.kate.id()] }));
    assert_eq!(
        lookup(&f, json!({ "discord": ["99999999999999999"] })).await,
        json!({ "discord": [] })
    );
    assert_eq!(
        lookup(&f, json!({ "email": ["sam@example.com"] })).await,
        json!({ "discord": [] })
    );
    // BluMint's connection makes matches only through these endpoints, so
    // every match on it is one BluMint knows.
    for path in ["/v1/matches", "/v1/lobbies", "/v1/tournaments"] {
        let (status, refused) = f
            .bridge
            .post_keyed(&f.provider, path, json!({}), Some("generic"))
            .await;
        assert_eq!(status, StatusCode::FORBIDDEN, "{path}: {refused}");
    }
    // Only a blumint connection's credential reaches these endpoints.
    let other = f.bridge.provider("mock-a").await;
    let (status, _) = f
        .bridge
        .post(&other, "/v1/blumint/lookup", json!({ "discord": [KATE] }))
        .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
}

#[tokio::test]
async fn a_match_is_created_played_and_its_score_sent_to_blumint() {
    let f = fixture().await;
    // A player BluMint never looked up cannot be put in a match.
    let (status, refused) = create(&f, "create-1", 1).await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{refused}");
    assert_eq!(refusal(&refused), "player_unlinked");
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let mut alone = request(&f, 1);
    alone["teams"] = json!([team("kate", f.kate.id())]);
    let (status, refused) = create_with(&f, "create-1", alone).await;
    assert_eq!(
        (status, refusal(&refused)),
        (StatusCode::BAD_REQUEST, "match_unsupported")
    );
    // The set's length is required and whole; one longer than Ember plays is
    // BluMint's to run instead.
    for (games, code) in [
        (json!(0), "invalid_request"),
        (json!(1.5), "invalid_request"),
        (json!("2"), "invalid_request"),
        (json!(11), "match_unsupported"),
        (json!(300), "match_unsupported"),
    ] {
        let mut body = request(&f, 1);
        body["series"]["gamesToWin"] = games.clone();
        let (status, refused) = create_with(&f, "create-1", body).await;
        assert_eq!(
            (status, refusal(&refused)),
            (StatusCode::BAD_REQUEST, code),
            "{games}"
        );
    }
    let mut endless = request(&f, 1);
    endless.as_object_mut().unwrap().remove("series");
    let (status, refused) = create_with(&f, "create-1", endless).await;
    assert_eq!(
        (status, refusal(&refused)),
        (StatusCode::BAD_REQUEST, "invalid_request")
    );
    // One player on both sides is refused, and nothing is created.
    let mut twice = request(&f, 1);
    twice["teams"][1] = team("kate again", f.kate.id());
    let (status, refused) = create_with(&f, "create-1", twice).await;
    assert_eq!(
        (status, refusal(&refused)),
        (StatusCode::BAD_REQUEST, "invalid_request")
    );
    assert_eq!(matches(&f).await, 0);

    // Spectators, team names, match settings and identities from other
    // sign-in methods are BluMint's own, and do not change the match.
    let mut body = request(&f, 1);
    body["spectators"] = json!([{ "identities": [ { "inGameId": "emb1_watcher" } ] }]);
    body["matchSettings"] = json!({ "gravity": 1.1 });
    body["teams"][0]["players"][0]["identities"]
        .as_array_mut()
        .unwrap()
        .push(json!({ "provider": "discord", "inGameId": KATE }));
    let (status, created) = create_with(&f, "create-1", body).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    let id = created["matchId"].as_str().unwrap().to_owned();
    // BluMint lost the answer and asks again under the same key: the same
    // match, even with its teams renamed. The key cannot ask for another
    // set, and a new key for the same players is refused while the match is
    // on. A first to 4 (best of 7) passes the length check.
    assert_eq!(
        create(&f, "create-1", 1).await,
        (StatusCode::OK, created.clone())
    );
    // Each registration issues BluMint a new credential, and the key belongs
    // to the connection, so a replay under the new credential answers the
    // same match.
    let fresh = f.bridge.provider(CONNECTION).await;
    assert_eq!(
        f.bridge
            .post_keyed(
                &fresh,
                "/v1/blumint/matches",
                request(&f, 1),
                Some("create-1")
            )
            .await,
        (StatusCode::OK, created.clone())
    );
    let mut renamed = request(&f, 1);
    renamed["teams"][0]["name"] = json!("kate (sub)");
    assert_eq!(
        create_with(&f, "create-1", renamed).await,
        (StatusCode::OK, created.clone())
    );
    let (status, refused) = create(&f, "create-1", 4).await;
    assert_eq!(
        (status, refused),
        (StatusCode::CONFLICT, json!({ "code": "key_reused" }))
    );
    let (status, refused) = create(&f, "create-2", 4).await;
    assert_eq!(status, StatusCode::LOCKED, "{refused}");
    assert_eq!(
        refused,
        json!({ "code": "player_busy", "players": [f.kate.id(), f.sam.id()] })
    );
    let (status, refused) = f
        .bridge
        .post(&f.provider, "/v1/blumint/matches", request(&f, 1))
        .await;
    assert_eq!(
        (status, refused),
        (
            StatusCode::BAD_REQUEST,
            json!({ "code": "invalid_request" })
        )
    );
    assert_eq!(
        created["matchUrl"],
        format!("https://embernetplay.link/m#{}/{id}", f.bridge.bridge_id).as_str()
    );
    assert_eq!(match_state(&f, &id).await, "awaiting_players");

    // Nothing is sent before the match ends.
    ember_bridge::deliver_results(f.bridge.state()).await;
    assert!(submitted(&f).is_empty());
    // BluMint is down at first: the result is sent again later, once. The
    // bridge's own worker runs alongside these passes; either may send.
    f.blumint.lock().unwrap().fail = 1;
    win(&f, &id, 1, "game-1").await;
    assert_eq!(match_state(&f, &id).await, "completed");
    results::until(&f.bridge, &id, |(state, attempts)| {
        state == "retrying" && attempts == 1
    })
    .await;
    assert!(submitted(&f).is_empty());
    f.bridge.clock.advance(11);
    results::until(&f.bridge, &id, |(state, _)| state == "delivered").await;
    ember_bridge::deliver_results(f.bridge.state()).await;
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("delivered".into(), 2)
    );
    assert_eq!(
        submitted(&f),
        vec![json!({ "mode": "result", "matchId": id, "teams": teams(&f, [0, 1]) })]
    );
    // The key still answers the match it made, now that it is over.
    assert_eq!(create(&f, "create-1", 1).await, (StatusCode::OK, created));
}

#[tokio::test]
async fn registration_tells_blumint_where_to_call_with_a_working_credential() {
    let f = fixture().await;
    ember_bridge::register_blumint(f.bridge.state(), CONNECTION)
        .await
        .unwrap();
    let calls = f.blumint.lock().unwrap().calls.clone();
    let urls: Vec<_> = calls
        .iter()
        .map(|(path, key, body)| {
            assert_eq!(key, "bm-key");
            assert!(body.get("retrieveMatchStatusUrl").is_none(), "{body}");
            let field = ["lookupPlayerUrl", "createMatchUrl", "closeMatchUrl"]
                .into_iter()
                .find(|field| body.get(field).is_some())
                .unwrap();
            (
                path.clone(),
                body[field].as_str().unwrap().to_owned(),
                body["authentication"].clone(),
            )
        })
        .collect();
    let paths: Vec<_> = urls
        .iter()
        .map(|(path, url, _)| (path.as_str(), url.clone()))
        .collect();
    assert_eq!(
        paths,
        [
            (
                "/api/tournaments/setLookupPlayer",
                f.bridge.url("/v1/blumint/lookup")
            ),
            (
                "/api/tournaments/match/setCreate",
                f.bridge.url("/v1/blumint/matches")
            ),
            (
                "/api/tournaments/match/setClose",
                f.bridge.url("/v1/blumint/matches/close")
            ),
        ]
    );
    // BluMint stores a credential per route, so every row carries the same
    // working one, as its secret and never as `apiKey`.
    let token = urls[0].2["secret"].as_str().unwrap().to_owned();
    for (path, _, authentication) in &urls {
        assert_eq!(
            authentication,
            &json!({
                "config": { "location": "header", "key": "Authorization", "valuePrefix": "Bearer " },
                "secret": token,
            }),
            "{path}"
        );
    }
    let (status, _) = f
        .bridge
        .post(&token, "/v1/blumint/lookup", json!({ "discord": [KATE] }))
        .await;
    assert_eq!(status, StatusCode::OK);
    // A connection that is not BluMint's is refused.
    assert!(
        ember_bridge::register_blumint(f.bridge.state(), "mock-a")
            .await
            .is_err()
    );
}

// BluMint keeps one credential per route, so once a registration has handed
// every route a new one, the one an earlier registration handed over calls
// nothing. A registration that fails partway leaves every credential working,
// since BluMint may hold either on each route.
#[tokio::test]
async fn a_new_registration_retires_the_credential_an_earlier_one_handed_over() {
    let f = fixture().await;
    let secret = || {
        f.blumint.lock().unwrap().calls.last().unwrap().2["authentication"]["secret"]
            .as_str()
            .unwrap()
            .to_owned()
    };
    let f = &f;
    let calls_with = |token: String| async move {
        f.bridge
            .post(&token, "/v1/blumint/lookup", json!({ "discord": [KATE] }))
            .await
            .0
    };
    // BluMint's close route is not there yet.
    f.blumint
        .lock()
        .unwrap()
        .answers
        .extend([(200, None), (200, None), (404, None)]);
    assert!(
        ember_bridge::register_blumint(f.bridge.state(), CONNECTION)
            .await
            .is_err()
    );
    let first = secret();
    assert_eq!(calls_with(first.clone()).await, StatusCode::OK);
    ember_bridge::register_blumint(f.bridge.state(), CONNECTION)
        .await
        .unwrap();
    let second = secret();
    assert_ne!(first, second);
    assert_eq!(calls_with(first).await, StatusCode::UNAUTHORIZED);
    assert_eq!(calls_with(second.clone()).await, StatusCode::OK);
    f.blumint
        .lock()
        .unwrap()
        .answers
        .extend([(200, None), (503, None)]);
    assert!(
        ember_bridge::register_blumint(f.bridge.state(), CONNECTION)
            .await
            .is_err()
    );
    let third = secret();
    assert_eq!(calls_with(second).await, StatusCode::OK);
    assert_eq!(calls_with(third).await, StatusCode::OK);
    // The test's own provider credential is not BluMint's callback one.
    assert_eq!(calls_with(f.provider.clone()).await, StatusCode::OK);
}

// BluMint looks up every player a refusal names and asks again, so a
// creation with two players it must look up again names both at once.
#[tokio::test]
async fn an_unlinked_seat_is_refused_naming_every_unlinked_player() {
    let f = fixture().await;
    let (status, refused) = create(&f, "create-1", 2).await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{refused}");
    assert_eq!(
        refused,
        json!({ "code": "player_unlinked", "players": [f.kate.id(), f.sam.id()] })
    );
    // A refusal is not stored against the key: BluMint asks again under it.
    lookup(&f, json!({ "discord": [SAM] })).await;
    let (_, refused) = create(&f, "create-1", 2).await;
    assert_eq!(
        refused,
        json!({ "code": "player_unlinked", "players": [f.kate.id()] })
    );
    assert_eq!(matches(&f).await, 0);
    lookup(&f, json!({ "discord": [KATE] })).await;
    let (status, created) = create(&f, "create-1", 2).await;
    assert_eq!(status, StatusCode::OK, "{created}");
}

// Every refusal BluMint branches on carries its code at the top level, with
// nothing else BluMint would have to read.
#[tokio::test]
async fn blumint_refusals_carry_the_registry_code_at_the_top_level() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let mut pair = request(&f, 2);
    let sam = pair["teams"][1]["players"][0].clone();
    pair["teams"][0]["players"]
        .as_array_mut()
        .unwrap()
        .push(sam);
    let mut doubled = request(&f, 2);
    doubled["teams"][0]["players"][0]["identities"]
        .as_array_mut()
        .unwrap()
        .push(json!({ "inGameId": f.sam.id() }));
    let mut nameless = request(&f, 2);
    nameless["teams"][1]["players"][0]["identities"] =
        json!([{ "provider": "discord", "inGameId": SAM }]);
    // The host's match settings are free-form, so a body can outgrow even
    // the creation's own cap.
    let mut oversized = request(&f, 2);
    oversized["matchSettings"] = json!({ "notes": "n".repeat(257 * 1024) });
    for (body, code) in [
        (pair, "match_unsupported"),
        (doubled, "invalid_request"),
        (nameless, "invalid_request"),
        (oversized, "invalid_request"),
        (json!({ "teams": "kate and sam" }), "invalid_request"),
    ] {
        let (status, refused) = create_with(&f, "create-1", body).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{refused}");
        assert_eq!(refused, json!({ "code": code }));
    }
    let response = f
        .bridge
        .client
        .post(f.bridge.url("/v1/blumint/matches"))
        .bearer_auth(&f.provider)
        .header("content-type", "application/json")
        .body("{ not json")
        .send()
        .await
        .unwrap();
    let (status, refused) = common::read(response).await;
    assert_eq!(
        (status, refused),
        (
            StatusCode::BAD_REQUEST,
            json!({ "code": "invalid_request" })
        )
    );
    assert_eq!(matches(&f).await, 0);

    // Long match settings, past the 64 KiB a close may carry, still make a
    // match.
    let mut long = request(&f, 2);
    long["matchSettings"] = json!({ "notes": "n".repeat(200 * 1024) });
    let (status, created) = create_with(&f, "create-2", long).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    assert_eq!(matches(&f).await, 1);
}

// BluMint holds a match whose player is still in another one, and asks again
// under the same key until that match ends, so the refusal names only the
// players who are busy, and is not stored against the key.
#[tokio::test]
async fn a_busy_player_is_refused_until_their_match_ends() {
    let f = fixture().await;
    let mut lou = f.bridge.player(3);
    f.bridge.open_session(&mut lou).await;
    let lou_id = lou.id().as_str().to_owned();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES ('99', ?1, 'lou', 1)",
                [lou_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 1).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    let against_lou = |first: &Player, second: &Player| {
        let mut body = request(&f, 1);
        body["teams"] = json!([team("one", first.id()), team("two", second.id())]);
        body
    };
    // A player to look up again is named before a busy one, so BluMint looks
    // them up at once rather than holding the match first.
    let (status, refused) = create_with(&f, "set-2", against_lou(&f.kate, &lou)).await;
    assert_eq!(
        (status, refused),
        (
            StatusCode::BAD_REQUEST,
            json!({ "code": "player_unlinked", "players": [lou.id()] })
        )
    );
    lookup(&f, json!({ "discord": ["99"] })).await;
    let (status, refused) = create_with(&f, "set-2", against_lou(&f.kate, &lou)).await;
    assert_eq!(
        (status, refused),
        (
            StatusCode::LOCKED,
            json!({ "code": "player_busy", "players": [f.kate.id()] })
        )
    );
    let (status, refused) = create_with(&f, "set-3", against_lou(&lou, &f.sam)).await;
    assert_eq!(
        (status, refused),
        (
            StatusCode::LOCKED,
            json!({ "code": "player_busy", "players": [f.sam.id()] })
        )
    );
    assert_eq!(matches(&f).await, 1);
    win(&f, &id, 0, "game-1").await;
    let (status, next) = create_with(&f, "set-2", against_lou(&f.kate, &lou)).await;
    assert_eq!(status, StatusCode::OK, "{next}");
    assert_ne!(next["matchId"], id.as_str());
}

// BluMint closes a match it no longer needs by the key it created it under,
// and waits for the 200 before it creates the players' next one. A closed
// match frees its players and is sent nothing more; a closed key makes no
// match from then on.
#[tokio::test]
async fn a_closed_match_sends_nothing() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 0, "game-1").await;
    assert_eq!(reports(&f, &id).await, vec![progressed(&f, &id, [1, 0])]);
    let mut revisions = Vec::new();
    for _ in 0..2 {
        let (status, closed) = close(
            &f,
            json!({ "idempotencyKey": "set-1", "reason": "decided" }),
        )
        .await;
        assert_eq!(status, StatusCode::OK, "{closed}");
        revisions.push(status_revision(&f, &id).await);
    }
    assert_eq!(revisions[0], revisions[1]);
    assert_eq!(match_state(&f, &id).await, "cancelled");
    assert_eq!(reports(&f, &id).await, vec![progressed(&f, &id, [1, 0])]);
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("not_required".into(), 0)
    );
    // The log names the credential that closed it.
    let closed_by: Vec<(String, String)> = f
        .bridge
        .state()
        .db
        .read(|tx| {
            Ok(tx
                .prepare("SELECT actor_class, actor_id FROM audit_entries WHERE action = 'blumint.close'")?
                .query_map([], |row| Ok((row.get(0)?, row.get(1)?)))?
                .collect::<rusqlite::Result<Vec<_>>>()?)
        })
        .await
        .unwrap();
    let provider: String = f
        .bridge
        .state()
        .db
        .read(|tx| {
            Ok(tx.query_row(
                "SELECT id FROM service_credentials WHERE connection_id = ?1 AND label = 'test provider'",
                [CONNECTION],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap();
    assert_eq!(closed_by, vec![("service".to_owned(), provider)]);

    // Whatever the reason, the match closes.
    for (key, reason) in [("set-2", "hostOverride"), ("set-3", "a reason from later")] {
        let (_, created) = create(&f, key, 2).await;
        let id = created["matchId"].as_str().unwrap().to_owned();
        let (status, closed) = close(&f, json!({ "idempotencyKey": key, "reason": reason })).await;
        assert_eq!(status, StatusCode::OK, "{closed}");
        assert_eq!(match_state(&f, &id).await, "cancelled");
        assert!(reports(&f, &id).await.is_empty());
    }

    // The match made under the key closes, and the key makes no match
    // again. The key is the connection's, so a credential from a later
    // registration closes it too.
    let (_, created) = create(&f, "set-4", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    let fresh = f.bridge.provider(CONNECTION).await;
    let (status, closed) = f
        .bridge
        .post_keyed(
            &fresh,
            "/v1/blumint/matches/close",
            json!({ "idempotencyKey": "set-4", "reason": "removed" }),
            Some("close-set-4"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{closed}");
    assert_eq!(match_state(&f, &id).await, "cancelled");
    assert_eq!(
        create(&f, "set-4", 2).await,
        (StatusCode::CONFLICT, json!({ "code": "key_reused" }))
    );
    // A key whose every creation was refused made no match, and closes all
    // the same, so a creation still on its way makes nothing.
    let made = matches(&f).await;
    assert_eq!(
        create(&f, "set-5", 11).await,
        (
            StatusCode::BAD_REQUEST,
            json!({ "code": "match_unsupported" })
        )
    );
    let (status, closed) = close(
        &f,
        json!({ "idempotencyKey": "set-5", "reason": "replaced" }),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{closed}");
    assert_eq!(
        create(&f, "set-5", 2).await,
        (StatusCode::CONFLICT, json!({ "code": "key_reused" }))
    );
    // A key the bridge holds nothing under is closed too, and answers as a
    // match already over does, since BluMint reads anything else as a
    // failed close. Asking again, as BluMint does when the first answer is
    // lost, answers the same, also after a creation the bridge refuses
    // arrives between.
    for refused_between in [true, false] {
        assert_eq!(
            close(
                &f,
                json!({ "idempotencyKey": "set-6", "reason": "replaced" })
            )
            .await,
            (StatusCode::OK, json!({ "closed": true }))
        );
        if refused_between {
            assert_eq!(
                create(&f, "set-6", 11).await,
                (
                    StatusCode::BAD_REQUEST,
                    json!({ "code": "match_unsupported" })
                )
            );
        }
    }
    // A known key's repeated close answers `200` each time.
    let (status, closed) = close(
        &f,
        json!({ "idempotencyKey": "set-5", "reason": "replaced" }),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{closed}");
    assert_eq!(
        create(&f, "set-6", 2).await,
        (StatusCode::CONFLICT, json!({ "code": "key_reused" }))
    );
    assert_eq!(matches(&f).await, made);
    assert!(reports(&f, &id).await.is_empty());

    // A close names the key, never the match.
    for body in [
        json!({ "matchId": id, "reason": "decided" }),
        json!({ "reason": "decided" }),
        json!({ "idempotencyKey": "set-4" }),
        json!({ "idempotencyKey": "set-4", "reason": "" }),
        json!({ "idempotencyKey": "set-4", "reason": "r".repeat(65) }),
        json!({ "idempotencyKey": "", "reason": "decided" }),
        json!({ "idempotencyKey": "k".repeat(129), "reason": "decided" }),
        json!({ "idempotencyKey": "set-4", "reason": "decided", "padding": "p".repeat(70 * 1024) }),
    ] {
        let (status, refused) = close(&f, body.clone()).await;
        assert_eq!(
            (status, refused),
            (
                StatusCode::BAD_REQUEST,
                json!({ "code": "invalid_request" })
            ),
            "{body}"
        );
    }
}

// The answer under a creation key is forgotten after a day, but the key is
// kept for as long as its match, as the match's own external ID: a close
// under it still ends the match, and it still makes no other.
#[tokio::test]
async fn a_close_finds_its_match_once_the_creation_answer_is_forgotten() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    let (_, found) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(found["external_match_id"], "blumint:set-1", "{found}");
    // The answer is a day old, and the maintenance pass removes it.
    f.bridge
        .state()
        .db
        .write(|tx| {
            tx.execute(
                "UPDATE idempotency_records SET created_at = created_at - 86400",
                [],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    ember_bridge::maintain(f.bridge.state()).await;
    let answers: i64 = f
        .bridge
        .state()
        .db
        .read(|tx| {
            Ok(tx.query_row(
                "SELECT COUNT(*) FROM idempotency_records WHERE path = '/v1/blumint/matches'",
                [],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap();
    assert_eq!(answers, 0);
    assert_eq!(match_state(&f, &id).await, "awaiting_players");
    assert_eq!(
        create(&f, "set-1", 2).await,
        (StatusCode::CONFLICT, json!({ "code": "key_reused" }))
    );

    let (status, closed) = close(
        &f,
        json!({ "idempotencyKey": "set-1", "reason": "removed" }),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{closed}");
    assert_eq!(match_state(&f, &id).await, "cancelled");
    assert_eq!(
        create(&f, "set-1", 2).await,
        (StatusCode::CONFLICT, json!({ "code": "key_reused" }))
    );
    assert_eq!(matches(&f).await, 1);
}

// A close names only a key its own connection created under. Two BluMint
// connections of one tenant (staging and production, say) can send the same
// key: a close from one ends nothing the other made or refused under it, and
// each connection's key makes and closes only its own match.
#[tokio::test]
async fn a_close_naming_another_connections_key_ends_nothing() {
    let f = fixture_adjusted(
        keyed(),
        config::DEFAULT_MATCH_EXPIRY_HOURS,
        second_connection,
    )
    .await;
    let other = f.bridge.provider(OTHER).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (status, found) = f
        .bridge
        .post(
            &other,
            "/v1/blumint/lookup",
            json!({ "discord": [KATE, SAM] }),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{found}");
    let (status, made) = f
        .bridge
        .post_keyed(&other, "/v1/blumint/matches", request(&f, 2), Some("set-1"))
        .await;
    assert_eq!(status, StatusCode::OK, "{made}");
    let theirs = made["matchId"].as_str().unwrap().to_owned();
    let (status, _) = f
        .bridge
        .post_keyed(
            &other,
            "/v1/blumint/matches",
            request(&f, 11),
            Some("set-2"),
        )
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let state = async || {
        let (_, found) = f.bridge.get(&other, &format!("/v1/matches/{theirs}")).await;
        (found["state"].clone(), found["revision"].clone())
    };
    let before = state().await;

    for key in ["set-1", "set-2"] {
        assert_eq!(
            close(&f, json!({ "idempotencyKey": key, "reason": "decided" })).await,
            (StatusCode::OK, json!({ "closed": true })),
            "{key}"
        );
    }
    assert_eq!(state().await, before);
    let (status, created) = create(&f, "set-3", 2).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    let ours = created["matchId"].as_str().unwrap().to_owned();
    let (status, closed) = close(
        &f,
        json!({ "idempotencyKey": "set-3", "reason": "decided" }),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{closed}");
    assert_eq!(match_state(&f, &ours).await, "cancelled");
    assert_eq!(state().await, before);
}

// A match that has ended answers a close with 200 and stays as it is: a
// completed set whose result BluMint has not taken yet is still sent it.
#[tokio::test]
async fn closing_a_match_that_has_ended_changes_nothing() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, "set-1", 1).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    f.blumint.lock().unwrap().fail = 1;
    win(&f, &id, 0, "game-1").await;
    results::until(&f.bridge, &id, |(state, attempts)| {
        state == "retrying" && attempts == 1
    })
    .await;
    let before = status_revision(&f, &id).await;
    let (status, closed) = close(
        &f,
        json!({ "idempotencyKey": "set-1", "reason": "decided" }),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{closed}");
    assert_eq!(match_state(&f, &id).await, "completed");
    assert_eq!(status_revision(&f, &id).await, before);
    f.bridge.clock.advance(11);
    results::until(&f.bridge, &id, |(state, _)| state == "delivered").await;
    assert_eq!(submitted(&f), vec![result(&f, &id, [1, 0])]);
}

// A bridge from before closes by key named its BluMint matches by an ID of
// its own, which no close can name. Each one still open when the bridge
// starts again ends as a close ends it: its players are free, and BluMint is
// sent nothing more for it, not even the score of a game it was not sent
// yet. A match made under a key goes on.
#[tokio::test]
async fn an_upgrade_ends_the_matches_blumint_cannot_close() {
    let mut f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    let lou = discord_player(&f, 3, "99", "lou").await;
    let lee = discord_player(&f, 4, "98", "lee").await;
    lookup(&f, json!({ "discord": [KATE, SAM, "99", "98"] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let old = created["matchId"].as_str().unwrap().to_owned();
    let mut body = request(&f, 2);
    body["teams"] = json!([team("lou", lou.id()), team("lee", lee.id())]);
    let (_, created) = create_with(&f, "set-2", body).await;
    let keyed = created["matchId"].as_str().unwrap().to_owned();
    let id = old.clone();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE matches SET external_match_id = 'blumint_old' WHERE id = ?1",
                [&id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    win(&f, &old, 0, "game-1").await;
    assert_eq!(progress(&f, &old).await, vec![(1, "queued".to_owned())]);
    let before = (
        match_state(&f, &keyed).await,
        status_revision(&f, &keyed).await,
    );

    f.bridge.restart(Secrets::default()).await;
    ember_bridge::deliver_results(&keyed_state(&f)).await;
    assert_eq!(match_state(&f, &old).await, "cancelled");
    assert_eq!(results::delivery(&f.bridge, &old).await.0, "not_required");
    assert_eq!(
        progress(&f, &old).await,
        vec![(1, "not_required".to_owned())]
    );
    assert!(submitted(&f).is_empty(), "{:?}", submitted(&f));
    let id = old.clone();
    let recorded = f
        .bridge
        .state()
        .db
        .read(move |tx| {
            let reasons = tx
                .prepare(
                    "SELECT json_extract(CAST(body AS TEXT), '$.data.reason') FROM events
                     WHERE match_id = ?1 AND type LIKE '%.match.cancelled.v1'",
                )?
                .query_map([&id], |row| row.get::<_, String>(0))?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            let actor: (String, String) = tx.query_row(
                "SELECT actor_class, actor_id FROM audit_entries WHERE action = 'blumint.close' AND target = ?1",
                [&id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?;
            Ok((reasons, actor))
        })
        .await
        .unwrap();
    assert_eq!(
        recorded,
        (
            vec!["closed on upgrade: made before BluMint closed matches by key".to_owned()],
            ("bridge".to_owned(), "upgrade".to_owned())
        )
    );
    assert_eq!(
        (
            match_state(&f, &keyed).await,
            status_revision(&f, &keyed).await
        ),
        before
    );
    let (status, next) = create(&f, "set-3", 2).await;
    assert_eq!(status, StatusCode::OK, "{next}");
}

// A status the bridge stored but cannot read fails on its own: the other
// matches' statuses still go out, and the match's next change writes it
// again from the status as it stands.
#[tokio::test]
async fn a_stored_status_that_does_not_parse_fails_alone() {
    let f = fixture_with(Secrets::default(), config::DEFAULT_MATCH_EXPIRY_HOURS).await;
    let keyed = keyed_state(&f);
    let lou = discord_player(&f, 3, "99", "lou").await;
    let lee = discord_player(&f, 4, "98", "lee").await;
    lookup(&f, json!({ "discord": [KATE, SAM, "99", "98"] })).await;
    let (_, created) = create(&f, "set-1", 2).await;
    let broken = created["matchId"].as_str().unwrap().to_owned();
    let mut body = request(&f, 2);
    body["teams"] = json!([team("lou", lou.id()), team("lee", lee.id())]);
    let (_, created) = create_with(&f, "set-2", body).await;
    let other = created["matchId"].as_str().unwrap().to_owned();
    claim(&f, &f.kate, &broken, &endpoint(&f.kate), BUILD).await;
    claim(&f, &lou, &other, &endpoint(&lou), BUILD).await;
    let id = broken.clone();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE blumint_status SET body = '{ not json' WHERE match_id = ?1",
                [&id],
            )?;
            Ok(())
        })
        .await
        .unwrap();

    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(status_row(&f, &broken).await, Some((1, "failed".into())));
    assert!(statuses(&f, &broken).is_empty());
    assert_eq!(statuses(&f, &other).len(), 1);

    let (status, claimed) = claim(&f, &f.sam, &broken, &endpoint(&f.sam), BUILD).await;
    assert_eq!(status, StatusCode::OK, "{claimed}");
    assert_eq!(status_row(&f, &broken).await, Some((2, "queued".into())));
    ember_bridge::deliver_results(&keyed).await;
    let sent = statuses(&f, &broken);
    assert_eq!(sent.len(), 1, "{sent:?}");
    assert_eq!(sent[0]["revision"], 2);
}
