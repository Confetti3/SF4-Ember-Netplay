//! The notifier against the specification's webhook fixture, and end to end:
//! bridge → signed webhook → notifier → fake Discord and Twitch endpoints.
use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
    time::Duration,
};

use axum::{Router, body::Bytes, http::HeaderMap, routing::post};
use ember_bridge::{AppState, Config as BridgeConfig, Db, Keys, config};
use ember_notifier::{Clock, Config, Discord, Notifier, Twitch};
use ember_protocol::{event::Event, json, webhook::Headers};
use serde_json::{Value, json};

fn examples() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../docs/design/identity-bridge/examples")
}

fn temp(name: &str) -> PathBuf {
    let mut bytes = [0u8; 8];
    getrandom::fill(&mut bytes).unwrap();
    let dir = std::env::temp_dir().join(format!(
        "ember-notifier-{name}-{}",
        ember_protocol::encoding::b64u(&bytes)
    ));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

#[test]
fn accepts_the_specification_fixture_once() {
    let fixture: Value =
        serde_json::from_slice(&std::fs::read(examples().join("webhook-fixture.json")).unwrap())
            .unwrap();
    let body = std::fs::read(examples().join("match-completed-event.json")).unwrap();
    let dir = temp("fixture");
    let clock = Clock::default();
    clock.advance(fixture["verification_time_unix"].as_i64().unwrap() - clock.now() as i64);
    let notifier = Notifier::new(
        Config {
            listen: "127.0.0.1:0".into(),
            bridge_origin: "https://bridge.ember.example".into(),
            secrets: vec![fixture["secret_base64"].as_str().unwrap().into()],
            database: dir.join("inbox.sqlite3"),
            discord: None,
            twitch: None,
            names: [(
                "emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja".to_owned(),
                "Player A".to_owned(),
            )]
            .into(),
        },
        clock.clone(),
    )
    .unwrap();
    let headers = Headers {
        id: fixture["headers"]["webhook-id"].as_str().unwrap().into(),
        timestamp: fixture["headers"]["webhook-timestamp"]
            .as_str()
            .unwrap()
            .into(),
        signature: fixture["headers"]["webhook-signature"]
            .as_str()
            .unwrap()
            .into(),
    };
    assert_eq!(notifier.accept(&headers, &body), Ok(true));
    // The same event again is acknowledged and not recorded twice (WEB-04).
    assert_eq!(notifier.accept(&headers, &body), Ok(false));
    let mut tampered = body.clone();
    tampered.push(b' ');
    assert!(notifier.accept(&headers, &tampered).is_err());
    let mut swapped = headers.clone();
    swapped.id = "evt_dddddddd-dddd-4ddd-8ddd-dddddddddddd".into();
    assert!(notifier.accept(&swapped, &body).is_err());
    clock.advance(301);
    assert!(notifier.accept(&headers, &body).is_err());

    let event: Event = json::parse_as(&body, 65536).unwrap();
    let message = notifier.render(&event).unwrap();
    assert!(
        message.text.starts_with("Player A wins the set."),
        "{}",
        message.text
    );
    let _ = std::fs::remove_dir_all(&dir);
}

/// A lobby event as the bridge would send it.
fn lobby_event(kind: &str, data: Value) -> Event {
    serde_json::from_value(json!({
        "specversion": "1.0",
        "id": "evt_eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "source": "https://bridge.ember.example",
        "type": format!("io.ember.tournament.{kind}.v1"),
        "subject": "lobbies/elb_1",
        "time": "2026-10-01T00:00:00Z",
        "datacontenttype": "application/json",
        "dataschema": format!("urn:ember:identity-tournament:v1:{kind}"),
        "emberseq": "1",
        "data": data,
    }))
    .unwrap()
}

#[test]
fn announces_lobby_rotations() {
    let fixture: Value =
        serde_json::from_slice(&std::fs::read(examples().join("webhook-fixture.json")).unwrap())
            .unwrap();
    let dir = temp("lobby");
    let king = "emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";
    let notifier = Notifier::new(
        Config {
            listen: "127.0.0.1:0".into(),
            bridge_origin: "https://bridge.ember.example".into(),
            secrets: vec![fixture["secret_base64"].as_str().unwrap().into()],
            database: dir.join("inbox.sqlite3"),
            discord: None,
            twitch: None,
            names: [(king.to_owned(), "Player A".to_owned())].into(),
        },
        Clock::default(),
    )
    .unwrap();
    let created = notifier
        .render(&lobby_event(
            "lobby.created",
            json!({ "lobby_id": "elb_1", "games_to_win": 2, "rotation": "winner_stays", "metadata": { "title": "Stream night" } }),
        ))
        .unwrap();
    assert_eq!(created.title, "Stream night");
    assert!(
        created.text.contains("first to 2, winner stays"),
        "{}",
        created.text
    );
    let completed = notifier
        .render(&lobby_event(
            "lobby.set.completed",
            json!({ "lobby_id": "elb_1", "winner_id": king, "streak": { "ember_id": king, "sets": 3 }, "queue": [] }),
        ))
        .unwrap();
    assert_eq!(
        completed.text,
        "Player A wins the set, 3 sets in a row. Nobody is waiting in the queue."
    );
    // Queue changes are not announced; the set's own match events say who plays.
    assert!(
        notifier
            .render(&lobby_event(
                "lobby.queue.changed",
                json!({ "lobby_id": "elb_1" })
            ))
            .is_none()
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn announces_tournament_progress() {
    let fixture: Value =
        serde_json::from_slice(&std::fs::read(examples().join("webhook-fixture.json")).unwrap())
            .unwrap();
    let dir = temp("tournament");
    let a = "emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";
    let notifier = Notifier::new(
        Config {
            listen: "127.0.0.1:0".into(),
            bridge_origin: "https://bridge.ember.example".into(),
            secrets: vec![fixture["secret_base64"].as_str().unwrap().into()],
            database: dir.join("inbox.sqlite3"),
            discord: None,
            twitch: None,
            names: [(a.to_owned(), "Player A".to_owned())].into(),
        },
        Clock::default(),
    )
    .unwrap();
    let created = notifier
        .render(&lobby_event(
            "tournament.created",
            json!({ "tournament_id": "etn_1", "format": "double_elimination", "games_to_win": 2,
                    "finals_games_to_win": 3, "metadata": { "title": "Friday night" } }),
        ))
        .unwrap();
    assert_eq!(created.title, "Friday night");
    assert_eq!(
        created.text,
        "Registration is open: double elimination, first to 2, finals first to 3."
    );
    let b = "emb1_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    let advanced = notifier
        .render(&lobby_event(
            "tournament.match.completed",
            json!({ "tournament_id": "etn_1", "label": "Winners round 1", "walkover": false,
                    "winner": { "ember_id": a }, "loser": { "ember_id": b },
                    "eliminated": null, "winner_next": "Winners final", "loser_next": "Losers round 1" }),
        ))
        .unwrap();
    assert!(
        advanced
            .text
            .starts_with("Player A advances to Winners final. ")
            && advanced.text.ends_with(" drops to Losers round 1."),
        "{}",
        advanced.text
    );
    let done = notifier
        .render(&lobby_event(
            "tournament.completed",
            json!({ "tournament_id": "etn_1", "placements": [
                { "ember_id": a, "placement": 1 },
                { "ember_id": b, "placement": 2 },
            ] }),
        ))
        .unwrap();
    assert!(
        done.text.starts_with("Champion: Player A. 2nd: "),
        "{}",
        done.text
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[derive(Clone, Default)]
struct Captured(Arc<Mutex<Vec<(HeaderMap, Value)>>>);

/// A fake endpoint that records each post and answers `answer`.
async fn fake(path: &'static str, answer: &'static str) -> (String, Captured) {
    let captured = Captured::default();
    let sink = captured.clone();
    let app = Router::new().route(
        path,
        post(move |headers: HeaderMap, body: Bytes| {
            let sink = sink.clone();
            async move {
                sink.0
                    .lock()
                    .unwrap()
                    .push((headers, serde_json::from_slice(&body).unwrap()));
                (axum::http::StatusCode::OK, answer)
            }
        }),
    );
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    (format!("http://{address}"), captured)
}

async fn call(origin: &str, token: &str, path: &str, key: &str, body: Value) -> Value {
    let response = reqwest::Client::new()
        .post(format!("{origin}{path}"))
        .bearer_auth(token)
        .header("content-type", "application/json")
        .header("idempotency-key", key)
        .body(serde_json::to_vec(&body).unwrap())
        .send()
        .await
        .unwrap();
    let status = response.status();
    let text = response.text().await.unwrap();
    assert!(status.is_success(), "{path}: {status} {text}");
    serde_json::from_str(&text).unwrap()
}

#[tokio::test]
async fn announces_a_set_on_discord_and_twitch() {
    let dir = temp("e2e");
    // The bridge.
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    let mut bytes = [0u8; 16];
    getrandom::fill(&mut bytes).unwrap();
    let bridge_config = BridgeConfig {
        bridge_id: ember_protocol::encoding::prefixed_id("brg", bytes),
        display_name: "Bridge".into(),
        origin: origin.clone(),
        listen: "127.0.0.1:0".into(),
        database: dir.join("bridge.sqlite3"),
        secrets: dir.join("secrets.json"),
        allow_loopback_http: true,
        allow_private_webhooks: true,
        mock_browser: false,
        discord: None,
        integration_secrets: None,
        rooms: None,
        tenants: vec![config::Tenant {
            id: "local".into(),
            name: "Local".into(),
            connections: vec![config::Connection {
                id: "mock-local".into(),
                kind: "mock".into(),
                environment: "local".into(),
                display_name: "Mock".into(),
                enabled: true,
                api_base: None,
            }],
        }],
    };
    let state = AppState::new(
        bridge_config,
        Keys::generate(),
        Default::default(),
        Db::open(&dir.join("bridge.sqlite3")).unwrap(),
        Default::default(),
    );
    ember_bridge::sync_config(&state).await.unwrap();
    let running = ember_bridge::start(state, listener).unwrap();
    let organizer = ember_bridge::issue_credential(&running.state, None, Some("local"), "notifier")
        .await
        .unwrap();

    // Fake Discord and Twitch, then the notifier.
    let (discord_origin, discord) = fake("/api/webhooks/1/token", "{}").await;
    let (twitch_origin, twitch) = fake(
        "/helix/chat/messages",
        r#"{"data":[{"message_id":"m","is_sent":true,"drop_reason":null}]}"#,
    )
    .await;
    let notifier_listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let notifier_url = format!("http://{}/webhook", notifier_listener.local_addr().unwrap());
    let subscription = call(
        &origin,
        &organizer,
        "/v1/webhook-subscriptions",
        "sub",
        json!({
            "url": notifier_url,
            "event_types": [
                "io.ember.tournament.match.created.v1",
                "io.ember.tournament.match.score.changed.v1",
                "io.ember.tournament.match.completed.v1",
            ],
        }),
    )
    .await;
    let notifier = Notifier::new(
        Config {
            listen: "127.0.0.1:0".into(),
            bridge_origin: origin.clone(),
            secrets: vec![subscription["secret"].as_str().unwrap().into()],
            database: dir.join("inbox.sqlite3"),
            discord: Some(Discord {
                webhook_url: format!("{discord_origin}/api/webhooks/1/token"),
                username: "Ember".into(),
            }),
            twitch: Some(Twitch {
                api_base: twitch_origin,
                client_id: "test-client".into(),
                token: "test-token".into(),
                broadcaster_id: "1001".into(),
                sender_id: "2002".into(),
            }),
            names: Default::default(),
        },
        Clock::default(),
    )
    .unwrap();
    let (_, tasks) = notifier.start(notifier_listener).unwrap();

    // Two linked players and an FT1 set decided by an organizer.
    let provider =
        ember_bridge::issue_credential(&running.state, Some("mock-local"), None, "provider")
            .await
            .unwrap();
    let participants = link_two(&origin, &provider).await;
    let created = call(
        &origin,
        &provider,
        "/v1/matches",
        "m1",
        json!({
            "external_match_id": "grand-final",
            "game": "usf4",
            "participants": participants,
            "rules": {
                "games_to_win": 1,
                "draw_policy": "replay_no_score",
                "native_rules_profile": "organizer-reported-v1",
                "edition_policy": "ultra_only",
                "character_policy": "unrestricted_between_games",
                "stage_policy": "p1_selects",
                "input_delay_policy": "ember_existing_ready_policy",
            },
            "observer_policy": "authorized_only",
            "result_policy": "two_player_agreement_or_review",
            "required_build_id": "test",
            "metadata": { "round_label": "Grand final" },
        }),
    )
    .await;
    let id = created["match_id"].as_str().unwrap();
    call(
        &origin,
        &organizer,
        &format!("/v1/matches/{id}/adjudications"),
        "g1",
        json!({ "kind": "game_result", "winner_slot": 0, "reason": "Reported on stream", "expected_revision": "1" }),
    )
    .await;

    for _ in 0..200 {
        if discord.0.lock().unwrap().len() >= 3 && twitch.0.lock().unwrap().len() >= 3 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    let posts = discord.0.lock().unwrap().clone();
    assert_eq!(posts.len(), 3, "{posts:?}");
    assert_eq!(posts[0].1["embeds"][0]["title"], "Grand final");
    assert_eq!(posts[0].1["allowed_mentions"], json!({ "parse": [] }));
    assert!(
        posts[2].1["embeds"][0]["description"]
            .as_str()
            .unwrap()
            .contains("wins the set")
    );
    let chats = twitch.0.lock().unwrap().clone();
    assert_eq!(chats.len(), 3);
    assert_eq!(chats[0].0["authorization"], "Bearer test-token");
    assert_eq!(chats[0].0["client-id"], "test-client");
    assert_eq!(chats[2].1["broadcaster_id"], "1001");
    assert!(
        chats[2].1["message"]
            .as_str()
            .unwrap()
            .starts_with("Grand final: ")
    );
    for task in tasks {
        task.abort();
    }
    running.abort();
    let _ = std::fs::remove_dir_all(&dir);
}

/// Links two throwaway identities through the provider-proxy route and
/// returns them as match participants.
async fn link_two(origin: &str, provider: &str) -> Vec<Value> {
    use ember_protocol::{
        SigningIdentity,
        challenge::{Action, Challenge, Expected, Method, ProvenRequest, command_digest},
        encoding::OriginPolicy,
    };
    let client = reqwest::Client::new();
    let bridge_id = {
        let text = client
            .get(format!("{origin}/.well-known/ember-bridge.json"))
            .send()
            .await
            .unwrap()
            .text()
            .await
            .unwrap();
        serde_json::from_str::<Value>(&text).unwrap()["bridge_id"]
            .as_str()
            .unwrap()
            .to_owned()
    };
    let post = |path: String, token: Option<String>, body: Vec<u8>| {
        let mut request = client
            .post(format!("{origin}{path}"))
            .header("content-type", "application/json")
            .body(body);
        if let Some(token) = token {
            request = request.bearer_auth(token);
        }
        async move {
            let response = request.send().await.unwrap();
            let status = response.status();
            let text = response.text().await.unwrap();
            assert!(status.is_success(), "{status} {text}");
            text
        }
    };
    let mut participants = Vec::new();
    for (slot, seed) in [(0u8, 41u8), (1, 42)] {
        let identity = SigningIdentity::from_seed(&zeroize::Zeroizing::new([seed; 32])).unwrap();
        let prove = async |token: Option<String>, action: Action, path: &str, command: Value| {
            let command = json::to_value(&command).unwrap();
            let request = json!({
                "public_key": identity.public_key(), "action": action, "method": Method::Post,
                "path": path, "request_digest": command_digest(&command),
            });
            let text = post(
                "/v1/auth/challenges".into(),
                token,
                serde_json::to_vec(&request).unwrap(),
            )
            .await;
            let challenge: Challenge = json::parse_as(text.as_bytes(), 8192).unwrap();
            let expected = Expected {
                bridge_id: &bridge_id,
                audience: origin,
                ember_id: identity.ember_id(),
                action,
                method: Method::Post,
                path,
                command: &command,
            };
            let now = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_secs();
            let proof = challenge
                .sign(&identity, &expected, now, OriginPolicy::AllowLoopbackHttp)
                .unwrap();
            ProvenRequest { command, proof }.to_json().unwrap()
        };
        let body = prove(
            None,
            Action::SessionCreate,
            "/v1/sessions",
            json!({ "requested_scopes": ["self:read"] }),
        )
        .await;
        let session: Value =
            serde_json::from_str(&post("/v1/sessions".into(), None, body).await).unwrap();
        let token = session["session_token"].as_str().unwrap().to_owned();
        let subject = format!("player-{slot}");
        let intent: Value = serde_json::from_str(
            &post(
                "/v1/link-intents".into(),
                Some(provider.into()),
                serde_json::to_vec(&json!({ "subject": subject })).unwrap(),
            )
            .await,
        )
        .unwrap();
        let body = prove(
            Some(token.clone()),
            Action::LinkClaim,
            "/v1/link-claims",
            json!({ "code": intent["code"], "connection_id": "mock-local", "consent": true }),
        )
        .await;
        let claim: Value =
            serde_json::from_str(&post("/v1/link-claims".into(), Some(token), body).await).unwrap();
        let link: Value = serde_json::from_str(
            &post(
                format!("/v1/link-intents/{}/approve", intent["intent_id"].as_str().unwrap()),
                Some(provider.into()),
                serde_json::to_vec(&json!({ "claim_id": claim["claim_id"], "ember_id": identity.ember_id(), "subject": subject })).unwrap(),
            )
            .await,
        )
        .unwrap();
        participants.push(json!({ "participant_id": link["participant_id"], "ember_id": identity.ember_id(), "slot": slot }));
    }
    participants
}
