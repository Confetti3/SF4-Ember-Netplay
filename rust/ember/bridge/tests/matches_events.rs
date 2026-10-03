//! Matches, adjudication, events, SSE and webhook delivery (RESULT-09,
//! RESULT-10, RESULT-19, PROVIDER-02, WEB-01 to WEB-06).
mod common;

use std::time::Duration;

use common::{
    Bridge, Player, code,
    receiver::{receiver, wait_for},
    types,
};
use ember_protocol::{
    matches::{DeliveryState, MatchCompleted},
    webhook::{self, Secret},
};
use reqwest::StatusCode;
use serde_json::{Value, json};

struct Fixture {
    bridge: Bridge,
    provider: String,
    organizer: String,
    a: Player,
    b: Player,
    links: [Value; 2],
}

async fn fixture() -> Fixture {
    let bridge = Bridge::start().await;
    let provider = bridge.provider("mock-a").await;
    let organizer = bridge.organizer("t1").await;
    let mut a = bridge.player(21);
    let mut b = bridge.player(22);
    bridge.open_session(&mut a).await;
    bridge.open_session(&mut b).await;
    let link_a = bridge.link(&provider, &a, "mock-a", "player-a").await;
    let link_b = bridge.link(&provider, &b, "mock-a", "player-b").await;
    Fixture {
        bridge,
        provider,
        organizer,
        a,
        b,
        links: [link_a, link_b],
    }
}

fn create_body(f: &Fixture, external: &str, profile: &str) -> Value {
    json!({
        "external_match_id": external,
        "game": "usf4",
        "participants": [
            { "participant_id": f.links[0]["participant_id"], "ember_id": f.a.id(), "slot": 0 },
            { "participant_id": f.links[1]["participant_id"], "ember_id": f.b.id(), "slot": 1 },
        ],
        "rules": {
            "games_to_win": 2,
            "draw_policy": "replay_no_score",
            "native_rules_profile": profile,
            "edition_policy": "ultra_only",
            "character_policy": "unrestricted_between_games",
            "stage_policy": "p1_selects",
            "input_delay_policy": "ember_existing_ready_policy",
        },
        "observer_policy": "authorized_only",
        "result_policy": "two_player_agreement_or_review",
        "required_build_id": "test-build",
        "metadata": { "round_label": "Winners round 1" },
    })
}

async fn create_match(f: &Fixture) -> (String, Value) {
    let (status, created) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/matches",
            create_body(f, "set-1", "organizer-reported-v1"),
            Some("create-1"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    (created["match_id"].as_str().unwrap().to_owned(), created)
}

async fn adjudicate(f: &Fixture, id: &str, key: &str, body: Value) -> (StatusCode, Value) {
    f.bridge
        .post_keyed(
            &f.organizer,
            &format!("/v1/matches/{id}/adjudications"),
            body,
            Some(key),
        )
        .await
}

// PROVIDER-02, spec 13.1
#[tokio::test]
async fn match_creation_is_validated_and_idempotent() {
    let f = fixture().await;
    let (status, refused) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/matches",
            create_body(&f, "set-1", "usf4-standard-v1"),
            Some("k0"),
        )
        .await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::UNPROCESSABLE_ENTITY, "unsupported_rules")
    );
    let (status, _) = f
        .bridge
        .post(
            &f.provider,
            "/v1/matches",
            create_body(&f, "set-1", "organizer-reported-v1"),
        )
        .await;
    assert_eq!(
        status,
        StatusCode::BAD_REQUEST,
        "an Idempotency-Key is required"
    );

    let (id, created) = create_match(&f).await;
    // A retry with the same key replays the original response.
    let (status, replay) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/matches",
            create_body(&f, "set-1", "organizer-reported-v1"),
            Some("create-1"),
        )
        .await;
    assert_eq!((status, &replay), (StatusCode::CREATED, &created));
    // A new key for the same logical match still returns that match.
    let (status, same) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/matches",
            create_body(&f, "set-1", "organizer-reported-v1"),
            Some("create-2"),
        )
        .await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(same["match_id"], id.as_str());
    // Reusing the key or the external ID for different content is refused.
    let mut changed = create_body(&f, "set-1", "organizer-reported-v1");
    changed["metadata"]["round_label"] = "Grand final".into();
    let (status, conflict) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/matches",
            changed.clone(),
            Some("create-1"),
        )
        .await;
    assert_eq!(
        (status, code(&conflict)),
        (StatusCode::CONFLICT, "idempotency_conflict")
    );
    let (status, conflict) = f
        .bridge
        .post_keyed(&f.provider, "/v1/matches", changed, Some("create-3"))
        .await;
    assert_eq!(
        (status, code(&conflict)),
        (StatusCode::CONFLICT, "idempotency_conflict")
    );
    // An active participant cannot be double-booked on the connection.
    let (status, busy) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/matches",
            create_body(&f, "set-2", "organizer-reported-v1"),
            Some("create-4"),
        )
        .await;
    assert_eq!(
        (status, code(&busy)),
        (StatusCode::CONFLICT, "lease_conflict")
    );

    // Participants must be the connection's current links.
    let stranger = f.bridge.player(23);
    let mut body = create_body(&f, "set-3", "organizer-reported-v1");
    body["participants"][1]["ember_id"] = json!(stranger.id());
    let (status, _) = f
        .bridge
        .post_keyed(&f.provider, "/v1/matches", body, Some("create-5"))
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let other_provider = f.bridge.provider("mock-b").await;
    let (status, _) = f
        .bridge
        .post_keyed(
            &other_provider,
            "/v1/matches",
            create_body(&f, "set-1", "organizer-reported-v1"),
            Some("x"),
        )
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);

    // Assigned players see the match; others cannot tell it exists.
    let (status, snapshot) = f
        .bridge
        .get(f.a.token(), &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(snapshot["state"], "awaiting_players");
    assert!(snapshot["event_cursor"].as_str().is_some());
    let (_, assignments) = f.bridge.get(f.b.token(), "/v1/assignments").await;
    assert_eq!(assignments["assignments"][0]["match_id"], id.as_str());
    let mut outsider = f.bridge.player(24);
    f.bridge.open_session(&mut outsider).await;
    let (status, _) = f
        .bridge
        .get(outsider.token(), &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let foreign = f.bridge.organizer("t2").await;
    let (status, _) = f.bridge.get(&foreign, &format!("/v1/matches/{id}")).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
}

// RESULT-07, RESULT-09, RESULT-10, RESULT-19, SEC-07
#[tokio::test]
async fn ft2_set_scores_once_per_game() {
    let f = fixture().await;
    let (id, _) = create_match(&f).await;
    let game = |slot: Option<u8>, revision: u64| {
        let mut body = json!({ "kind": "game_result", "reason": "Reported by both players on stream", "expected_revision": revision.to_string() });
        match slot {
            Some(slot) => body["winner_slot"] = slot.into(),
            None => body["draw"] = true.into(),
        }
        body
    };
    // Providers and players cannot adjudicate; only organizers can.
    let (status, _) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/matches/{id}/adjudications"),
            game(Some(0), 1),
            Some("p"),
        )
        .await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _) = f
        .bridge
        .post_keyed(
            f.a.token(),
            &format!("/v1/matches/{id}/adjudications"),
            game(Some(0), 1),
            Some("p"),
        )
        .await;
    assert_eq!(status, StatusCode::FORBIDDEN);

    let (status, first) = adjudicate(&f, &id, "g1", game(Some(0), 1)).await;
    assert_eq!(status, StatusCode::CREATED, "{first}");
    // The same request retried adds no second win.
    let (_, retry) = adjudicate(&f, &id, "g1", game(Some(0), 1)).await;
    assert_eq!(retry, first);
    // A stale revision is refused.
    let (status, stale) = adjudicate(&f, &id, "g2", game(Some(1), 1)).await;
    assert_eq!(
        (status, code(&stale)),
        (StatusCode::CONFLICT, "stale_revision")
    );
    let (_, draw) = adjudicate(&f, &id, "g2", game(None, 2)).await;
    assert_eq!(draw["scores"], first["scores"]);
    let (_, second) = adjudicate(&f, &id, "g3", game(Some(1), 3)).await;
    assert_eq!(second["state"], "between_games");
    let (_, done) = adjudicate(&f, &id, "g4", game(Some(0), 4)).await;
    assert_eq!(done["state"], "completed");
    assert_eq!(done["scores"][0]["wins"], 2);
    assert_eq!(done["scores"][1]["wins"], 1);

    let (_, snapshot) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(snapshot["attempts"].as_array().unwrap().len(), 4);
    let events = f.bridge.events(&f.organizer, "0").await;
    let kinds = types(&events);
    let completed: Vec<_> = events
        .iter()
        .filter(|event| event["type"] == "io.ember.tournament.match.completed.v1")
        .collect();
    assert_eq!(completed.len(), 1, "{kinds:?}");
    let data: MatchCompleted = serde_json::from_value(completed[0]["data"].clone()).unwrap();
    data.check().unwrap();
    assert_eq!(data.winner_id, *f.a.id());
    assert_eq!(data.accepted_attempt_ids.len(), 3);
    // This provider reads results itself; the bridge sends it none.
    assert_eq!(snapshot["provider_delivery_state"], "not_required");
    assert_eq!(data.provider_delivery_state, DeliveryState::NotRequired);
    assert_eq!(
        kinds
            .iter()
            .filter(|k| k.ends_with("match.game.confirmed.v1"))
            .count(),
        4
    );
    // Players see their own match's events as well.
    assert_eq!(
        f.bridge.events(f.b.token(), "0").await.len() - 2,
        events.len()
    );

    // A finished match only changes through an explicit correction.
    let (status, _) = adjudicate(&f, &id, "g5", game(Some(1), 5)).await;
    assert_eq!(status, StatusCode::CONFLICT);
    let last = snapshot["attempts"][3]["attempt_id"].as_str().unwrap();
    // Reopening it would double-book a player who has started another match.
    let (status, next_set) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/matches",
            create_body(&f, "set-2", "organizer-reported-v1"),
            Some("create-next"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{next_set}");
    let void = json!({ "kind": "void_game", "attempt_id": last, "reason": "Wrong game recorded", "expected_revision": "5" });
    let (status, refused) = adjudicate(&f, &id, "v0", void.clone()).await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "lease_conflict")
    );
    let next_id = next_set["match_id"].as_str().unwrap();
    let (status, _) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/matches/{next_id}/cancel"),
            json!({ "reason": "Correction first", "expected_revision": "1" }),
            Some("cancel-next"),
        )
        .await;
    assert_eq!(status, StatusCode::OK);
    let (status, corrected) = adjudicate(
        &f,
        &id,
        "v1",
        json!({ "kind": "void_game", "attempt_id": last, "reason": "Wrong game recorded", "expected_revision": "5" }),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED, "{corrected}");
    assert_eq!(corrected["state"], "between_games");
    let kinds = types(&f.bridge.events(&f.organizer, "0").await);
    assert_eq!(
        kinds.last().unwrap(),
        "io.ember.tournament.match.corrected.v1"
    );
}

#[tokio::test]
async fn cancel_and_unlink_follow_policy() {
    let f = fixture().await;
    let (id, _) = create_match(&f).await;
    let path = format!("/v1/matches/{id}/cancel");
    let (status, _) = f
        .bridge
        .post_keyed(
            &f.provider,
            &path,
            json!({ "reason": "No show", "expected_revision": "9" }),
            Some("c1"),
        )
        .await;
    assert_eq!(status, StatusCode::CONFLICT);
    // Unlinking an assigned player sends the match to review.
    let link = f.links[1]["link_id"].as_str().unwrap();
    let removed = f
        .bridge
        .client
        .delete(f.bridge.url(&format!("/v1/links/{link}")))
        .bearer_auth(&f.provider)
        .send()
        .await
        .unwrap();
    assert_eq!(removed.status(), StatusCode::OK);
    let (_, snapshot) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(snapshot["state"], "needs_review");
    let (status, cancelled) = f
        .bridge
        .post_keyed(
            &f.provider,
            &path,
            json!({ "reason": "Player left", "expected_revision": "2" }),
            Some("c2"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{cancelled}");
    assert_eq!(cancelled["state"], "cancelled");
}

// WEB-01, WEB-02, WEB-03, WEB-04
#[tokio::test]
async fn webhooks_are_signed_retried_and_rotated() {
    let f = fixture().await;
    let (url, hook) = receiver().await;
    // Registration refuses metadata endpoints and credentials in URLs.
    for bad in [
        "http://169.254.169.254/latest",
        "http://user:pw@127.0.0.1/hook",
        "ftp://127.0.0.1/hook",
    ] {
        let (status, _) = f
            .bridge
            .post_keyed(
                &f.organizer,
                "/v1/webhook-subscriptions",
                json!({ "url": bad, "event_types": ["io.ember.tournament.match.created.v1"] }),
                Some(bad),
            )
            .await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{bad}");
    }
    // Organizers cannot subscribe to identity events.
    let (status, _) = f
        .bridge
        .post_keyed(&f.organizer, "/v1/webhook-subscriptions", json!({ "url": url, "event_types": ["io.ember.tournament.identity.link.completed.v1"] }), Some("id"))
        .await;
    assert_eq!(status, StatusCode::FORBIDDEN);

    let subscribe = json!({
        "url": url,
        "event_types": ["io.ember.tournament.match.created.v1", "io.ember.tournament.match.game.confirmed.v1"],
    });
    let (status, created) = f
        .bridge
        .post_keyed(
            &f.organizer,
            "/v1/webhook-subscriptions",
            subscribe.clone(),
            Some("sub-1"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    let secret = Secret::parse(created["secret"].as_str().unwrap()).unwrap();
    // A retry gets the same subscription without the secret.
    let (_, replay) = f
        .bridge
        .post_keyed(
            &f.organizer,
            "/v1/webhook-subscriptions",
            subscribe,
            Some("sub-1"),
        )
        .await;
    assert_eq!(replay["subscription_id"], created["subscription_id"]);
    assert!(replay["secret"].is_null());
    assert_eq!(replay["secret_redacted"], true);

    // The first delivery fails, the retry carries the same ID and body.
    hook.answer_with(&[503]);
    let (id, _) = create_match(&f).await;
    let deliveries = wait_for(&hook, 2).await;
    let (first, second) = (&deliveries[0], &deliveries[1]);
    assert_eq!(first.0.id, second.0.id);
    assert_eq!(first.1, second.1);
    let now = f.bridge.clock.now();
    webhook::verify(&[&secret], &second.0, &second.1, now).unwrap();
    let mut tampered = second.1.clone();
    tampered.push(b' ');
    assert!(webhook::verify(&[&secret], &second.0, &tampered, now).is_err());
    let event: Value = serde_json::from_slice(&second.1).unwrap();
    assert_eq!(event["type"], "io.ember.tournament.match.created.v1");
    // The delivered bytes are the event exactly as the log serves it.
    let logged = f.bridge.events(&f.organizer, "0").await;
    assert_eq!(
        serde_json::from_slice::<Value>(&second.1).unwrap(),
        logged[0]
    );

    // Rotation: deliveries carry old and new signatures during the overlap.
    let sub = created["subscription_id"].as_str().unwrap();
    let (status, rotated) = f
        .bridge
        .post(
            &f.organizer,
            &format!("/v1/webhook-subscriptions/{sub}/rotate-secret"),
            json!({ "overlap_seconds": 60 }),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{rotated}");
    let new_secret = Secret::parse(rotated["secret"].as_str().unwrap()).unwrap();
    adjudicate(&f, &id, "g1", json!({ "kind": "game_result", "winner_slot": 1, "reason": "Stream VOD", "expected_revision": "1" })).await;
    let deliveries = wait_for(&hook, 3).await;
    let third = &deliveries[2];
    let now = f.bridge.clock.now();
    webhook::verify(&[&secret], &third.0, &third.1, now).unwrap();
    webhook::verify(&[&new_secret], &third.0, &third.1, now).unwrap();
    // After the overlap only the new secret signs.
    f.bridge.clock.advance(61);
    adjudicate(&f, &id, "g2", json!({ "kind": "game_result", "winner_slot": 1, "reason": "Stream VOD", "expected_revision": "2" })).await;
    let deliveries = wait_for(&hook, 4).await;
    let fourth = &deliveries[3];
    let now = f.bridge.clock.now();
    assert!(webhook::verify(&[&secret], &fourth.0, &fourth.1, now).is_err());
    webhook::verify(&[&new_secret], &fourth.0, &fourth.1, now).unwrap();

    // Deleting the subscription stops delivery.
    let removed = f
        .bridge
        .client
        .delete(f.bridge.url(&format!("/v1/webhook-subscriptions/{sub}")))
        .bearer_auth(&f.organizer)
        .send()
        .await
        .unwrap();
    assert_eq!(removed.status(), StatusCode::OK);
}

// WEB-05, WEB-06
#[tokio::test]
async fn event_stream_resumes_and_hides_others() {
    let f = fixture().await;
    let response = f
        .bridge
        .client
        .get(f.bridge.url("/v1/events/stream"))
        .bearer_auth(f.a.token())
        .header("last-event-id", "0")
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    assert!(
        response.headers()["content-type"]
            .to_str()
            .unwrap()
            .starts_with("text/event-stream")
    );
    let mut response = response;
    let (id, _) = create_match(&f).await;
    let mut text = String::new();
    let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
    while !text.contains(&id) && tokio::time::Instant::now() < deadline {
        if let Ok(Ok(Some(chunk))) =
            tokio::time::timeout(Duration::from_secs(5), response.chunk()).await
        {
            text.push_str(&String::from_utf8_lossy(&chunk));
        }
    }
    assert!(
        text.contains("io.ember.tournament.identity.link.completed.v1"),
        "{text}"
    );
    assert!(text.contains(&id), "{text}");
    // Player A never sees player B's identity events.
    assert!(!text.contains(&f.links[1]["link_id"].as_str().unwrap().to_owned()));

    // Polling resumes after a cursor without repeating or skipping.
    let all = f.bridge.events(f.a.token(), "0").await;
    let cursor = all[0]["emberseq"].as_str().unwrap();
    let rest = f.bridge.events(f.a.token(), cursor).await;
    assert_eq!(rest.len(), all.len() - 1);
    assert_eq!(rest[0], all[1]);
    let (status, _) = f.bridge.get(f.a.token(), "/v1/events?after=01").await;
    assert_eq!(status, StatusCode::BAD_REQUEST);

    // Revoking the session ends the stream it opened.
    let revoke = f
        .bridge
        .client
        .delete(f.bridge.url("/v1/sessions/current"))
        .bearer_auth(f.a.token())
        .send()
        .await
        .unwrap();
    assert_eq!(revoke.status(), StatusCode::OK);
    let deadline = tokio::time::Instant::now() + Duration::from_secs(15);
    let mut ended = false;
    while tokio::time::Instant::now() < deadline {
        match tokio::time::timeout(Duration::from_secs(1), response.chunk()).await {
            Ok(Ok(None)) | Ok(Err(_)) => {
                ended = true;
                break;
            }
            _ => {}
        }
    }
    assert!(ended, "a revoked session's stream kept running");
}

// A subscription whose owner was revoked receives nothing, even if it is
// still marked enabled (a create that raced the revocation).
#[tokio::test]
async fn a_revoked_owners_webhook_receives_nothing() {
    let f = fixture().await;
    let (url, hook) = receiver().await;
    let (control_url, control) = receiver().await;
    let rogue = ember_bridge::issue_credential(f.bridge.state(), None, Some("t1"), "rogue")
        .await
        .unwrap();
    let subscribe =
        |url: &str| json!({ "url": url, "event_types": ["io.ember.tournament.match.created.v1"] });
    let (status, created) = f
        .bridge
        .post_keyed(
            &rogue,
            "/v1/webhook-subscriptions",
            subscribe(&url),
            Some("rogue"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    let (status, _) = f
        .bridge
        .post_keyed(
            &f.organizer,
            "/v1/webhook-subscriptions",
            subscribe(&control_url),
            Some("control"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED);

    let credentials = ember_bridge::list_credentials(f.bridge.state())
        .await
        .unwrap();
    let rogue_id = credentials
        .iter()
        .find(|c| c.label == "rogue")
        .unwrap()
        .id
        .clone();
    assert!(
        ember_bridge::revoke_credential(f.bridge.state(), &rogue_id)
            .await
            .unwrap()
    );
    // The state a create committed just after the revocation would leave.
    let sub = created["subscription_id"].as_str().unwrap().to_owned();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            Ok(tx.execute(
                "UPDATE webhook_subscriptions SET enabled = 1 WHERE id = ?1",
                [sub],
            )?)
        })
        .await
        .unwrap();

    create_match(&f).await;
    wait_for(&control, 1).await;
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert!(hook.received().is_empty());
    // And a revoked credential cannot create another subscription.
    let (status, _) = f
        .bridge
        .post_keyed(
            &rogue,
            "/v1/webhook-subscriptions",
            subscribe(&url),
            Some("again"),
        )
        .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
}
