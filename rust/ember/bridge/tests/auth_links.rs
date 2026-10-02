//! Discovery, challenges, sessions and linking (AUTH-xx and LINK-xx).
mod common;

use common::{Bridge, code, read, types};
use ember_protocol::challenge::{Action, Method, ProvenRequest};
use reqwest::StatusCode;
use serde_json::json;

#[tokio::test]
async fn discovery_is_public_and_informational() {
    let bridge = Bridge::start().await;
    let bridge_ref = &bridge;
    let fetch = |path: &'static str| async move {
        let response = bridge_ref
            .client
            .get(bridge_ref.url(path))
            .send()
            .await
            .unwrap();
        read(response).await.1
    };
    let profile = fetch("/.well-known/ember-bridge.json").await;
    assert_eq!(profile["bridge_id"], bridge.bridge_id.as_str());
    assert_eq!(profile["origin"], bridge.origin.as_str());
    let caps = fetch("/v1/capabilities").await;
    assert_eq!(caps["native_play"], false);
    assert_eq!(caps["native_rules_profiles"], json!([]));
    let keys = fetch("/v1/signing-keys").await;
    assert_eq!(keys["keys"][0]["alg"], "EdDSA");
    assert_eq!(keys["keys"][0]["x"].as_str().unwrap().len(), 43);
}

// AUTH-06: a revoked service credential stops working at once.
#[tokio::test]
async fn revoked_credentials_stop_working() {
    let bridge = Bridge::start().await;
    let provider = bridge.provider("mock-a").await;
    let other = bridge.organizer("t1").await;
    let (status, _) = bridge.get(&provider, "/v1/webhook-subscriptions").await;
    assert_eq!(status, StatusCode::OK);

    let list = ember_bridge::list_credentials(bridge.state())
        .await
        .unwrap();
    assert_eq!(list.len(), 2);
    let issued = list.iter().find(|c| c.role == "provider").unwrap();
    assert_eq!(issued.connection_id.as_deref(), Some("mock-a"));
    assert!(issued.revoked_at.is_none());

    assert!(
        ember_bridge::revoke_credential(bridge.state(), &issued.id)
            .await
            .unwrap()
    );
    let (status, _) = bridge.get(&provider, "/v1/webhook-subscriptions").await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, _) = bridge.get(&other, "/v1/webhook-subscriptions").await;
    assert_eq!(status, StatusCode::OK);

    assert!(
        !ember_bridge::revoke_credential(bridge.state(), &issued.id)
            .await
            .unwrap()
    );
    assert!(
        ember_bridge::revoke_credential(bridge.state(), "cred_unknown")
            .await
            .is_err()
    );
    let list = ember_bridge::list_credentials(bridge.state())
        .await
        .unwrap();
    assert!(
        list.iter()
            .any(|c| c.id == issued.id && c.revoked_at.is_some())
    );
}

// AUTH-01, AUTH-03, AUTH-04, AUTH-06
#[tokio::test]
async fn sessions_need_a_fresh_single_use_proof() {
    let bridge = Bridge::start().await;
    let mut player = bridge.player(1);
    let command = json!({ "requested_scopes": ["self:read"] });
    let body = bridge
        .prove(
            &player,
            Action::SessionCreate,
            Method::Post,
            "/v1/sessions",
            command.clone(),
        )
        .await;
    let (status, created) = bridge
        .send_proof(&player, Method::Post, "/v1/sessions", body.clone())
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    assert_eq!(created["scopes"], json!(["self:read"]));
    // The same proof again is refused.
    let (status, again) = bridge
        .send_proof(&player, Method::Post, "/v1/sessions", body.clone())
        .await;
    assert_eq!(
        (status, code(&again)),
        (StatusCode::UNAUTHORIZED, "challenge_used")
    );

    // A proof for one command cannot carry another.
    let fresh = bridge
        .prove(
            &player,
            Action::SessionCreate,
            Method::Post,
            "/v1/sessions",
            command.clone(),
        )
        .await;
    let mut swapped = ProvenRequest::parse(&fresh).unwrap();
    swapped.command =
        common::value(json!({ "requested_scopes": ["self:read", "tournament:participate"] }));
    let (status, refused) = bridge
        .send_proof(
            &player,
            Method::Post,
            "/v1/sessions",
            swapped.to_json().unwrap(),
        )
        .await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::UNAUTHORIZED, "invalid_signature")
    );
    // ...and the refused attempt did not burn the challenge.
    let (status, _) = bridge
        .send_proof(&player, Method::Post, "/v1/sessions", fresh)
        .await;
    assert_eq!(status, StatusCode::CREATED);

    // An expired challenge cannot open a session.
    let late = bridge
        .prove(
            &player,
            Action::SessionCreate,
            Method::Post,
            "/v1/sessions",
            command.clone(),
        )
        .await;
    bridge.clock.advance(61);
    let (status, expired) = bridge
        .send_proof(&player, Method::Post, "/v1/sessions", late)
        .await;
    assert_eq!(
        (status, code(&expired)),
        (StatusCode::UNAUTHORIZED, "challenge_expired")
    );

    bridge.open_session(&mut player).await;
    let token = player.token().to_owned();
    let (status, identity) = bridge.get(&token, "/v1/identity").await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(identity["ember_id"], player.id().as_str());
    // Player sessions never work on provider APIs.
    let (status, _) = bridge
        .post(&token, "/v1/players/resolve", json!({ "subjects": ["a"] }))
        .await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    // Sessions expire after ten minutes and can be revoked sooner.
    bridge.clock.advance(601);
    let (status, _) = bridge.get(&token, "/v1/identity").await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    bridge.open_session(&mut player).await;
    let revoke = bridge
        .client
        .delete(bridge.url("/v1/sessions/current"))
        .bearer_auth(player.token())
        .send()
        .await
        .unwrap();
    assert_eq!(revoke.status(), StatusCode::OK);
    let (status, _) = bridge.get(player.token(), "/v1/identity").await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
}

// AUTH-05
#[tokio::test]
async fn strict_json_is_enforced_before_authorization() {
    let bridge = Bridge::start().await;
    for body in [
        r#"{"public_key":"x","public_key":"y","action":"session.create","method":"POST","path":"/v1/sessions","request_digest":"x"}"#,
        r#"{"command":{"requested_scopes":["self:read"]},"proof":{"challenge_id":"a","signature":"b"},"extra":1}"#,
        r#"{"command":{"n":1.0},"proof":{"challenge_id":"a","signature":"b"}}"#,
    ] {
        for path in ["/v1/auth/challenges", "/v1/sessions"] {
            let response = bridge
                .client
                .post(bridge.url(path))
                .header("content-type", "application/json")
                .body(body)
                .send()
                .await
                .unwrap();
            let (status, error) = read(response).await;
            assert_eq!(
                (status, code(&error)),
                (StatusCode::BAD_REQUEST, "invalid_request"),
                "{path} {body}"
            );
        }
    }
    let huge = format!(
        r#"{{"command":{{"pad":"{}"}},"proof":{{}}}}"#,
        "a".repeat(9000)
    );
    let response = bridge
        .client
        .post(bridge.url("/v1/sessions"))
        .body(huge)
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::BAD_REQUEST);
}

#[tokio::test]
async fn privileged_challenges_need_a_session_for_the_same_key() {
    let bridge = Bridge::start().await;
    let player = bridge.player(2);
    let response = bridge
        .client
        .post(bridge.url("/v1/auth/challenges"))
        .body(
            serde_json::to_vec(&json!({
                "public_key": player.identity.public_key(),
                "action": "link.claim",
                "method": "POST",
                "path": "/v1/link-claims",
                "request_digest": ember_protocol::encoding::b64u(&[0; 32]),
            }))
            .unwrap(),
        )
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::UNAUTHORIZED);
    // A challenge for a route the action does not cover is refused.
    let response = bridge
        .client
        .post(bridge.url("/v1/auth/challenges"))
        .body(
            serde_json::to_vec(&json!({
                "public_key": player.identity.public_key(),
                "action": "session.create",
                "method": "POST",
                "path": "/v1/link-claims",
                "request_digest": ember_protocol::encoding::b64u(&[0; 32]),
            }))
            .unwrap(),
        )
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::BAD_REQUEST);
}

// LINK-01 (provider proxy), LINK-04, LINK-06, LINK-11, LINK-12
#[tokio::test]
async fn provider_proxy_link_flow() {
    let bridge = Bridge::start().await;
    let provider = bridge.provider("mock-a").await;
    let mut player = bridge.player(3);
    bridge.open_session(&mut player).await;

    let subject = "76561198000000001"; // a 64-bit ID kept as an exact string
    let (status, intent) = bridge
        .post(
            &provider,
            "/v1/link-intents",
            json!({ "subject": subject, "display_label": "Alice" }),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{intent}");
    let code_text = intent["code"].as_str().unwrap().to_owned();
    assert_eq!(code_text.len(), 11);
    let intent_id = intent["intent_id"].as_str().unwrap().to_owned();

    let (status, claim) = bridge
        .claim(&player, &code_text.to_lowercase(), "mock-a")
        .await;
    assert_eq!(status, StatusCode::CREATED, "{claim}");
    assert_eq!(claim["state"], "pending");
    assert_eq!(claim["fingerprint"], player.id().fingerprint());
    // Retrying the same claim returns the same pending claim.
    let (_, again) = bridge.claim(&player, &code_text, "mock-a").await;
    assert_eq!(again["claim_id"], claim["claim_id"]);

    let (status, view) = bridge
        .get(&provider, &format!("/v1/link-intents/{intent_id}"))
        .await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(view["claim"]["ember_id"], player.id().as_str());
    // A pending link is not a link.
    let (_, links) = bridge.get(player.token(), "/v1/links").await;
    assert_eq!(links["links"], json!([]));
    assert_eq!(links["pending"][0]["state"], "waiting_for_browser_approval");

    let approve = |ember: serde_json::Value| json!({ "claim_id": claim["claim_id"], "ember_id": ember, "subject": subject });
    let other = bridge.player(4);
    let (status, mismatch) = bridge
        .post(
            &provider,
            &format!("/v1/link-intents/{intent_id}/approve"),
            approve(json!(other.id())),
        )
        .await;
    assert_eq!(
        (status, code(&mismatch)),
        (StatusCode::CONFLICT, "link_conflict")
    );
    let (status, link) = bridge
        .post(
            &provider,
            &format!("/v1/link-intents/{intent_id}/approve"),
            approve(json!(player.id())),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{link}");
    let (status, repeat) = bridge
        .post(
            &provider,
            &format!("/v1/link-intents/{intent_id}/approve"),
            approve(json!(player.id())),
        )
        .await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(repeat["link_id"], link["link_id"]);

    // A retry on another approved intent cannot name this claim to read its link.
    let second_subject = "76561198000000002";
    let (_, second) = bridge
        .post(
            &provider,
            "/v1/link-intents",
            json!({ "subject": second_subject, "display_label": "Bob" }),
        )
        .await;
    let second_id = second["intent_id"].as_str().unwrap().to_owned();
    let mut second_player = bridge.player(5);
    bridge.open_session(&mut second_player).await;
    let (_, second_claim) = bridge
        .claim(&second_player, second["code"].as_str().unwrap(), "mock-a")
        .await;
    let (status, _) = bridge
        .post(
            &provider,
            &format!("/v1/link-intents/{second_id}/approve"),
            json!({ "claim_id": second_claim["claim_id"], "ember_id": second_player.id(), "subject": second_subject }),
        )
        .await;
    assert_eq!(status, StatusCode::OK);
    let (status, leaked) = bridge
        .post(
            &provider,
            &format!("/v1/link-intents/{second_id}/approve"),
            json!({ "claim_id": claim["claim_id"], "ember_id": player.id(), "subject": second_subject }),
        )
        .await;
    assert_ne!(status, StatusCode::OK, "{leaked}");
    assert!(leaked.get("link_id").is_none());

    // The code is spent.
    let (status, spent) = bridge.claim(&player, &code_text, "mock-a").await;
    assert_eq!(
        (status, code(&spent)),
        (StatusCode::CONFLICT, "link_expired")
    );

    let (_, resolved) = bridge
        .post(
            &provider,
            "/v1/players/resolve",
            json!({ "subjects": [subject, "unknown"] }),
        )
        .await;
    assert_eq!(resolved["players"][0]["ember_id"], player.id().as_str());
    assert_eq!(
        resolved["players"][0]["participant_id"],
        link["participant_id"]
    );
    assert_eq!(resolved["players"][1]["linked"], false);
    // Another tenant's provider sees nothing of it.
    let foreign = bridge.provider("mock-c").await;
    let (_, nothing) = bridge
        .post(
            &foreign,
            "/v1/players/resolve",
            json!({ "subjects": [subject] }),
        )
        .await;
    assert_eq!(nothing["players"][0]["linked"], false);
    let (status, _) = bridge
        .get(&foreign, &format!("/v1/link-intents/{intent_id}"))
        .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    assert!(bridge.events(&foreign, "0").await.is_empty());

    let player_events = types(&bridge.events(player.token(), "0").await);
    assert_eq!(
        player_events,
        [
            "io.ember.tournament.identity.link.pending.v1",
            "io.ember.tournament.identity.link.completed.v1"
        ]
    );
    // Both links' pending and completed events.
    assert_eq!(bridge.events(&provider, "0").await.len(), 4);
    // An organizer sees match events only, never identity links.
    let organizer = bridge.organizer("t1").await;
    assert!(bridge.events(&organizer, "0").await.is_empty());
}

// LINK-02, LINK-05
#[tokio::test]
async fn a_stolen_code_cannot_finish_a_link() {
    let bridge = Bridge::start().await;
    let provider = bridge.provider("mock-a").await;
    let mut genuine = bridge.player(5);
    let mut attacker = bridge.player(6);
    bridge.open_session(&mut genuine).await;
    bridge.open_session(&mut attacker).await;
    let (_, intent) = bridge
        .post(&provider, "/v1/link-intents", json!({ "subject": "bob" }))
        .await;
    let code_text = intent["code"].as_str().unwrap();
    let intent_id = intent["intent_id"].as_str().unwrap();

    let (status, stolen) = bridge.claim(&attacker, code_text, "mock-a").await;
    assert_eq!(status, StatusCode::CREATED);
    // The pending claimant is never swapped for another key.
    let (status, blocked) = bridge.claim(&genuine, code_text, "mock-a").await;
    assert_eq!(
        (status, code(&blocked)),
        (StatusCode::CONFLICT, "link_pending_approval")
    );
    // The attacker has no way to approve: players cannot use account routes.
    let (status, _) = bridge
        .post(
            attacker.token(),
            &format!("/v1/link-intents/{intent_id}/approve"),
            json!({ "claim_id": stolen["claim_id"], "ember_id": attacker.id() }),
        )
        .await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    // The account holder sees the wrong fingerprint and rejects it.
    let (_, view) = bridge
        .get(&provider, &format!("/v1/link-intents/{intent_id}"))
        .await;
    assert_eq!(view["claim"]["fingerprint"], attacker.id().fingerprint());
    let (status, _) = bridge
        .post(
            &provider,
            &format!("/v1/link-intents/{intent_id}/reject"),
            json!({ "claim_id": stolen["claim_id"] }),
        )
        .await;
    assert_eq!(status, StatusCode::OK);
    let (status, claim) = bridge.claim(&genuine, code_text, "mock-a").await;
    assert_eq!(status, StatusCode::CREATED, "{claim}");
    let (status, _) = bridge
        .post(
            &provider,
            &format!("/v1/link-intents/{intent_id}/approve"),
            json!({ "claim_id": claim["claim_id"], "ember_id": genuine.id(), "subject": "bob" }),
        )
        .await;
    assert_eq!(status, StatusCode::OK);
    let (_, links) = bridge.get(attacker.token(), "/v1/links").await;
    assert_eq!(links["links"], json!([]));
}

// LINK-03, LINK-07, LINK-10
#[tokio::test]
async fn codes_are_scoped_and_bounded() {
    let bridge = Bridge::start().await;
    let provider = bridge.provider("mock-a").await;
    let mut player = bridge.player(7);
    bridge.open_session(&mut player).await;
    // A provider must name its verified subject; a player cannot make intents.
    let (status, _) = bridge.post(&provider, "/v1/link-intents", json!({})).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = bridge
        .post(
            player.token(),
            "/v1/link-intents",
            json!({ "subject": "x" }),
        )
        .await;
    assert_eq!(status, StatusCode::FORBIDDEN);

    let (_, intent) = bridge
        .post(&provider, "/v1/link-intents", json!({ "subject": "carol" }))
        .await;
    let code_text = intent["code"].as_str().unwrap();
    // The same code under another connection, environment or tenant fails,
    // with the same answer as an unknown code.
    let (status, wrong) = bridge.claim(&player, code_text, "mock-b").await;
    assert_eq!(
        (status, code(&wrong)),
        (StatusCode::CONFLICT, "link_expired")
    );
    let (_, unknown) = bridge.claim(&player, "00000-00000", "mock-a").await;
    assert_eq!(unknown["error"]["message"], wrong["error"]["message"]);
    for _ in 0..4 {
        bridge.claim(&player, code_text, "mock-c").await;
    }
    // Five misuses retire the code even for its own connection.
    let (status, retired) = bridge.claim(&player, code_text, "mock-a").await;
    assert_eq!(
        (status, code(&retired)),
        (StatusCode::CONFLICT, "link_expired")
    );

    // Expired codes never link.
    let (_, intent) = bridge
        .post(&provider, "/v1/link-intents", json!({ "subject": "carol" }))
        .await;
    bridge.clock.advance(301);
    bridge.open_session(&mut player).await;
    let (status, _) = bridge
        .claim(&player, intent["code"].as_str().unwrap(), "mock-a")
        .await;
    assert_eq!(status, StatusCode::CONFLICT);

    // Claim submissions per key are rate limited.
    let mut last = StatusCode::OK;
    for _ in 0..12 {
        last = bridge.claim(&player, "00000-00000", "mock-a").await.0;
    }
    assert_eq!(last, StatusCode::TOO_MANY_REQUESTS);
}

fn form(pairs: &[(&str, &str)]) -> String {
    url::form_urlencoded::Serializer::new(String::new())
        .extend_pairs(pairs)
        .finish()
}

async fn mock_login(bridge: &Bridge, connection: &str, subject: &str) -> String {
    let response = bridge
        .client
        .post(bridge.url(&format!("/mock/{connection}/login")))
        .header("content-type", "application/x-www-form-urlencoded")
        .body(form(&[("subject", subject), ("display_label", subject)]))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::SEE_OTHER);
    let cookie = response.headers()["set-cookie"].to_str().unwrap();
    assert!(cookie.contains("HttpOnly") && cookie.contains("SameSite=Strict"));
    cookie.split(';').next().unwrap().to_owned()
}

fn csrf_of(page: &str) -> String {
    let start = page.find("name=\"csrf\" value=\"").unwrap() + 19;
    page[start..start + page[start..].find('"').unwrap()].to_owned()
}

// LINK-01 (browser), LINK-08, LINK-09
#[tokio::test]
async fn browser_approval_and_replacement() {
    let bridge = Bridge::start().await;
    let cookie = mock_login(&bridge, "mock-a", "dana").await;
    let page = bridge
        .client
        .get(bridge.url("/mock/mock-a/link"))
        .header("cookie", &cookie)
        .send()
        .await
        .unwrap();
    assert!(
        page.headers()["content-security-policy"]
            .to_str()
            .unwrap()
            .contains("default-src 'none'")
    );
    let page = page.text().await.unwrap();
    let csrf = csrf_of(&page);

    let create = |csrf: Option<&str>| {
        let mut request = bridge
            .client
            .post(bridge.url("/v1/link-intents"))
            .header("cookie", &cookie)
            .body("{}");
        if let Some(csrf) = csrf {
            request = request.header("x-csrf-token", csrf);
        }
        request.send()
    };
    assert_eq!(create(None).await.unwrap().status(), StatusCode::FORBIDDEN);
    let (status, intent) = read(create(Some(&csrf)).await.unwrap()).await;
    assert_eq!(status, StatusCode::CREATED, "{intent}");

    let mut first = bridge.player(8);
    bridge.open_session(&mut first).await;
    let (_, claim) = bridge
        .claim(&first, intent["code"].as_str().unwrap(), "mock-a")
        .await;
    // The approval page shows the claimant's fingerprint.
    let page = bridge
        .client
        .get(bridge.url("/mock/mock-a/link"))
        .header("cookie", &cookie)
        .send()
        .await
        .unwrap()
        .text()
        .await
        .unwrap();
    assert!(page.contains(&first.id().fingerprint()));
    let approve = bridge
        .client
        .post(bridge.url(&format!(
            "/mock/mock-a/link/{}/approve",
            intent["intent_id"].as_str().unwrap()
        )))
        .header("cookie", &cookie)
        .header("content-type", "application/x-www-form-urlencoded")
        .body(form(&[
            ("csrf", &csrf),
            ("claim_id", claim["claim_id"].as_str().unwrap()),
            ("ember_id", first.id().as_str()),
        ]))
        .send()
        .await
        .unwrap();
    assert_eq!(approve.status(), StatusCode::SEE_OTHER);
    let (_, links) = bridge.get(first.token(), "/v1/links").await;
    assert_eq!(links["links"][0]["account_label"], "da***");
    let link_id = links["links"][0]["link_id"].as_str().unwrap().to_owned();

    // Replacing the link needs explicit replacement and a fresh sign-in;
    // this browser signed in more than five minutes ago by now.
    bridge.clock.advance(301);
    let mut second = bridge.player(9);
    bridge.open_session(&mut second).await;
    let (_, intent) = read(create(Some(&csrf)).await.unwrap()).await;
    let (_, claim) = bridge
        .claim(&second, intent["code"].as_str().unwrap(), "mock-a")
        .await;
    let approve_json = |replace: bool| {
        bridge
            .client
            .post(bridge.url(&format!("/v1/link-intents/{}/approve", intent["intent_id"].as_str().unwrap())))
            .header("cookie", &cookie)
            .header("x-csrf-token", &csrf)
            .body(serde_json::to_vec(&json!({ "claim_id": claim["claim_id"], "ember_id": second.id(), "replace": replace })).unwrap())
            .send()
    };
    let (status, conflict) = read(approve_json(false).await.unwrap()).await;
    assert_eq!(
        (status, code(&conflict)),
        (StatusCode::CONFLICT, "link_conflict")
    );
    let (status, stale) = read(approve_json(true).await.unwrap()).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{stale}");
    let fresh_cookie = mock_login(&bridge, "mock-a", "dana").await;
    let fresh_page = bridge
        .client
        .get(bridge.url("/mock/mock-a/link"))
        .header("cookie", &fresh_cookie)
        .send()
        .await
        .unwrap()
        .text()
        .await
        .unwrap();
    // The pending intent belongs to the older session, so the fresh session
    // makes its own.
    let fresh_csrf = csrf_of(&fresh_page);
    let (_, intent) = read(
        bridge
            .client
            .post(bridge.url("/v1/link-intents"))
            .header("cookie", &fresh_cookie)
            .header("x-csrf-token", &fresh_csrf)
            .body("{}")
            .send()
            .await
            .unwrap(),
    )
    .await;
    bridge.open_session(&mut second).await;
    let (_, claim) = bridge
        .claim(&second, intent["code"].as_str().unwrap(), "mock-a")
        .await;
    let (status, replaced) = read(
        bridge
            .client
            .post(bridge.url(&format!("/v1/link-intents/{}/approve", intent["intent_id"].as_str().unwrap())))
            .header("cookie", &fresh_cookie)
            .header("x-csrf-token", &fresh_csrf)
            .body(serde_json::to_vec(&json!({ "claim_id": claim["claim_id"], "ember_id": second.id(), "replace": true })).unwrap())
            .send()
            .await
            .unwrap(),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{replaced}");
    bridge.open_session(&mut first).await;
    let (_, links) = bridge.get(first.token(), "/v1/links").await;
    assert_eq!(links["links"], json!([]));
    let first_events = types(&bridge.events(first.token(), "0").await);
    assert!(first_events.contains(&"io.ember.tournament.identity.link.removed.v1".to_owned()));

    // Unlink with a fresh proof; the key and ID stay the same.
    let (_, links) = bridge.get(second.token(), "/v1/links").await;
    let second_link = links["links"][0]["link_id"].as_str().unwrap().to_owned();
    assert_ne!(second_link, link_id);
    let path = format!("/v1/links/{second_link}");
    let body = bridge
        .prove(
            &second,
            Action::LinkRemove,
            Method::Delete,
            &path,
            json!({ "link_id": second_link }),
        )
        .await;
    let (status, removed) = bridge
        .send_proof(&second, Method::Delete, &path, body)
        .await;
    assert_eq!(status, StatusCode::OK, "{removed}");
    let (_, identity) = bridge.get(second.token(), "/v1/identity").await;
    assert_eq!(identity["ember_id"], second.id().as_str());
    assert_eq!(identity["active_links"], 0);
}

#[tokio::test]
async fn players_cancel_their_own_pending_claims() {
    let bridge = Bridge::start().await;
    let provider = bridge.provider("mock-a").await;
    let mut player = bridge.player(10);
    bridge.open_session(&mut player).await;
    let (_, intent) = bridge
        .post(&provider, "/v1/link-intents", json!({ "subject": "erin" }))
        .await;
    let (_, claim) = bridge
        .claim(&player, intent["code"].as_str().unwrap(), "mock-a")
        .await;
    let claim_id = claim["claim_id"].as_str().unwrap();
    let path = format!("/v1/link-claims/{claim_id}/cancel");
    let body = bridge
        .prove(
            &player,
            Action::LinkCancel,
            Method::Post,
            &path,
            json!({ "claim_id": claim_id }),
        )
        .await;
    let (status, _) = bridge.send_proof(&player, Method::Post, &path, body).await;
    assert_eq!(status, StatusCode::OK);
    let (_, view) = bridge
        .get(
            &provider,
            &format!("/v1/link-intents/{}", intent["intent_id"].as_str().unwrap()),
        )
        .await;
    assert_eq!(view["state"], "created");
    assert!(view["claim"].is_null());

    // Once the account side approved, a cancellation is refused, not reported as done.
    let (_, claim) = bridge
        .claim(&player, intent["code"].as_str().unwrap(), "mock-a")
        .await;
    let claim_id = claim["claim_id"].as_str().unwrap();
    let (status, _) = bridge
        .post(
            &provider,
            &format!(
                "/v1/link-intents/{}/approve",
                intent["intent_id"].as_str().unwrap()
            ),
            json!({ "claim_id": claim_id, "ember_id": player.id(), "subject": "erin" }),
        )
        .await;
    assert_eq!(status, StatusCode::OK);
    let path = format!("/v1/link-claims/{claim_id}/cancel");
    let body = bridge
        .prove(
            &player,
            Action::LinkCancel,
            Method::Post,
            &path,
            json!({ "claim_id": claim_id }),
        )
        .await;
    let (status, refused) = bridge.send_proof(&player, Method::Post, &path, body).await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "link_conflict")
    );
    let (_, links) = bridge.get(player.token(), "/v1/links").await;
    assert_eq!(links["links"].as_array().unwrap().len(), 1);
}
