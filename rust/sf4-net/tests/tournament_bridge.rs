//! The helper's tournament worker against a real bridge on loopback: enable
//! an identity, approve the bridge, link through a signed claim, unlink, and
//! keep the same Ember ID across a worker restart.
#![cfg(windows)]

use std::{path::PathBuf, time::Duration};

use ember_bridge::{AppState, Clock, Config, Db, Keys, config};
use serde_json::{Value, json};
use sf4_net::{
    service::{Command, Event},
    tournament::{self, Request},
};
use tokio::sync::mpsc;

struct Dirs(PathBuf);

impl Drop for Dirs {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn random16() -> [u8; 16] {
    let mut bytes = [0; 16];
    getrandom::fill(&mut bytes).unwrap();
    bytes
}

async fn bridge(dir: &std::path::Path) -> (ember_bridge::Running, String) {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    let config = Config {
        bridge_id: ember_protocol::encoding::prefixed_id("brg", random16()),
        display_name: "Loopback bridge".into(),
        origin: origin.clone(),
        listen: "127.0.0.1:0".into(),
        database: dir.join("bridge.sqlite3"),
        secrets: dir.join("secrets.json"),
        allow_loopback_http: true,
        allow_private_webhooks: true,
        mock_browser: false,
        discord: None,
        integration_secrets: None,
        tenants: vec![config::Tenant {
            id: "local".into(),
            name: "Local".into(),
            connections: vec![config::Connection {
                id: "mock-local".into(),
                kind: "mock".into(),
                environment: "local".into(),
                display_name: "Mock provider".into(),
                enabled: true,
                api_base: None,
            }],
        }],
    };
    let db = Db::open(&config.database).unwrap();
    let state = AppState::new(config, Keys::generate(), Default::default(), db, Clock::default());
    ember_bridge::sync_config(&state).await.unwrap();
    (ember_bridge::start(state, listener).unwrap(), origin)
}

struct Worker {
    handle: tournament::Handle,
    events: mpsc::Receiver<Event>,
    task: tokio::task::JoinHandle<()>,
    next: u64,
}

impl Worker {
    fn start(dirs: &Dirs) -> Self {
        Self::start_as(dirs, "a".repeat(64))
    }

    /// A worker whose helper run has the Iroh endpoint `endpoint`.
    fn start_as(dirs: &Dirs, endpoint: String) -> Self {
        let (sender, events) = mpsc::channel(64);
        let (handle, task) = tournament::spawn_in(
            sender,
            dirs.0.join("identity"),
            dirs.0.join("tournament"),
            false,
            endpoint,
        );
        Self {
            handle,
            events,
            task,
            next: 100,
        }
    }

    async fn next_answer(&mut self, request_id: u64) -> (bool, Option<String>, Value, Value) {
        loop {
            let event = tokio::time::timeout(Duration::from_secs(30), self.events.recv())
                .await
                .expect("an answer")
                .expect("worker running");
            if let Event::Tournament {
                request_id: id,
                ok,
                reason,
                identity,
                data,
                ..
            } = event
                && id == request_id
            {
                return (
                    ok,
                    reason,
                    identity.unwrap_or(Value::Null),
                    data.unwrap_or(Value::Null),
                );
            }
        }
    }

    /// Sends `request` the way the native side does, as IPC JSON.
    async fn ask(&mut self, request: Value) -> (bool, Option<String>, Value, Value) {
        self.next += 1;
        let id = self.next;
        let command: Command =
            serde_json::from_value(json!({ "type": "tournament", "request": request })).unwrap();
        let Command::Tournament { request } = command else {
            unreachable!()
        };
        self.handle.submit(id, request);
        self.next_answer(id).await
    }
}

async fn provider_call(origin: &str, token: &str, path: &str, body: Value) -> Value {
    let response = reqwest::Client::new()
        .post(format!("{origin}{path}"))
        .bearer_auth(token)
        .header("content-type", "application/json")
        .header(
            "idempotency-key",
            ember_protocol::encoding::b64u(&random16()),
        )
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
async fn link_an_identity_through_the_helper() {
    let dirs = Dirs(std::env::temp_dir().join(format!(
        "sf4-tournament-e2e-{}",
        ember_protocol::encoding::b64u(&random16())
    )));
    std::fs::create_dir_all(&dirs.0).unwrap();
    let (running, origin) = bridge(&dirs.0).await;
    let provider = ember_bridge::issue_credential(&running.state, Some("mock-local"), None, "e2e")
        .await
        .unwrap();

    let mut worker = Worker::start(&dirs);
    // The startup status arrives unasked, and nothing exists yet.
    let (_, _, identity, _) = worker.next_answer(0).await;
    assert_eq!(identity["state"], "disabled");
    // Bridge work needs an identity first.
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "link_list", "bridge_id": "brg_00000000-0000-4000-8000-000000000000" }))
        .await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("bridge_not_approved"));

    let (ok, reason, identity, _) = worker.ask(json!({ "op": "identity_enable" })).await;
    assert!(ok, "{reason:?}");
    assert_eq!(identity["state"], "ready");
    let ember_id = identity["ember_id"].as_str().unwrap().to_owned();

    // A backup goes into a folder the game names but has not made.
    let backup = dirs
        .0
        .join("settings")
        .join("identity-backups")
        .join("ember.backup");
    let backup = backup.to_str().unwrap();
    let (ok, reason, _, written) = worker
        .ask(json!({ "op": "identity_export", "path": backup, "passphrase": "backup words" }))
        .await;
    assert!(ok, "{reason:?}");
    assert_eq!(written["path"], backup);
    let (ok, _, _, preview) = worker
        .ask(json!({ "op": "identity_preview_import", "path": backup, "passphrase": "backup words" }))
        .await;
    assert!(ok);
    assert_eq!(preview["ember_id"], ember_id.as_str());
    assert_eq!(preview["same_identity"], true);

    let (ok, _, _, inspected) = worker
        .ask(json!({ "op": "bridge_inspect", "origin": origin }))
        .await;
    assert!(ok);
    let bridge_id = inspected["profile"]["bridge_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(
        inspected["capabilities"]["connections"][0]["id"],
        "mock-local"
    );
    // Approval names the bridge ID the user saw; a different one is refused.
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "bridge_approve", "origin": origin, "bridge_id": "brg_00000000-0000-4000-8000-000000000000" }))
        .await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("bridge_changed"));
    let (ok, _, _, _) = worker
        .ask(json!({ "op": "bridge_approve", "origin": origin, "bridge_id": bridge_id }))
        .await;
    assert!(ok);

    let intent = provider_call(
        &origin,
        &provider,
        "/v1/link-intents",
        json!({ "subject": "kate", "display_label": "Kate" }),
    )
    .await;
    let (ok, reason, _, claim) = worker
        .ask(json!({ "op": "link_claim", "bridge_id": bridge_id, "connection_id": "mock-local", "code": intent["code"] }))
        .await;
    assert!(ok, "{reason:?}");
    assert_eq!(claim["state"], "pending");
    assert_eq!(claim["ember_id"], ember_id.as_str());
    // A wrong code comes back as the bridge's stable code.
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "link_claim", "bridge_id": bridge_id, "connection_id": "mock-local", "code": "00000-00000" }))
        .await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("link_expired"));

    provider_call(
        &origin,
        &provider,
        &format!(
            "/v1/link-intents/{}/approve",
            intent["intent_id"].as_str().unwrap()
        ),
        json!({ "claim_id": claim["claim_id"], "ember_id": ember_id, "subject": "kate" }),
    )
    .await;
    let (ok, _, _, links) = worker
        .ask(json!({ "op": "link_list", "bridge_id": bridge_id }))
        .await;
    assert!(ok);
    let link_id = links["links"][0]["link_id"].as_str().unwrap().to_owned();
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "link_remove", "bridge_id": bridge_id, "link_id": link_id }))
        .await;
    assert!(ok, "{reason:?}");
    let (_, _, _, links) = worker
        .ask(json!({ "op": "link_list", "bridge_id": bridge_id }))
        .await;
    assert_eq!(links["links"], json!([]));

    // A new worker on the same stores has the same identity and bridge.
    worker.task.abort();
    let mut restarted = Worker::start(&dirs);
    let (_, _, identity, _) = restarted.next_answer(0).await;
    assert_eq!(identity["state"], "ready");
    assert_eq!(identity["ember_id"], ember_id.as_str());
    let (_, _, _, bridges) = restarted.ask(json!({ "op": "bridge_list" })).await;
    assert_eq!(bridges["bridges"][0]["bridge_id"], bridge_id.as_str());
    let (ok, _, _, _) = restarted
        .ask(json!({ "op": "link_list", "bridge_id": bridge_id }))
        .await;
    assert!(ok);

    // Importing a different identity drops the session the old one opened:
    // the next claim is signed and sent as the new identity.
    let other_dirs = Dirs(dirs.0.join("other"));
    let mut other = Worker::start(&other_dirs);
    other.next_answer(0).await;
    let (_, _, other_identity, _) = other.ask(json!({ "op": "identity_enable" })).await;
    let other_id = other_identity["ember_id"].as_str().unwrap().to_owned();
    let other_backup = other_dirs.0.join("other.backup");
    let other_backup = other_backup.to_str().unwrap();
    let (ok, _, _, _) = other
        .ask(json!({ "op": "identity_export", "path": other_backup, "passphrase": "other words" }))
        .await;
    assert!(ok);
    other.task.abort();
    let (ok, reason, identity, _) = restarted
        .ask(
            json!({ "op": "identity_import", "path": other_backup, "passphrase": "other words",
            "expected_ember_id": other_id, "replace": true }),
        )
        .await;
    assert!(ok, "{reason:?}");
    assert_eq!(identity["ember_id"], other_id.as_str());
    let intent = provider_call(
        &origin,
        &provider,
        "/v1/link-intents",
        json!({ "subject": "kate-two", "display_label": "Kate" }),
    )
    .await;
    let (ok, reason, _, claim) = restarted
        .ask(json!({ "op": "link_claim", "bridge_id": bridge_id, "connection_id": "mock-local", "code": intent["code"] }))
        .await;
    assert!(ok, "{reason:?}");
    assert_eq!(claim["ember_id"], other_id.as_str());

    // Unknown fields and passphrase-free unlocks are refused at the IPC edge.
    let bad: Result<Command, _> = serde_json::from_value(
        json!({ "type": "tournament", "request": { "op": "identity_status", "extra": 1 } }),
    );
    assert!(bad.is_err());
    running.abort();
}

#[test]
fn requests_carry_no_debug_output_of_secrets() {
    // `Request` has no Debug derive; this only needs to compile without one.
    fn takes(_: &Request) {}
    let request: Request =
        serde_json::from_value(json!({ "op": "identity_unlock", "passphrase": "secret words" }))
            .unwrap();
    takes(&request);
}

/// One worker with an enabled identity, the bridge approved, and its
/// identity linked to `subject` on the mock connection.
async fn linked_worker(
    dirs: &Dirs,
    endpoint: &str,
    origin: &str,
    provider: &str,
    subject: &str,
) -> (Worker, String, String, Value) {
    let mut worker = Worker::start_as(dirs, endpoint.to_owned());
    worker.next_answer(0).await;
    let (_, _, identity, _) = worker.ask(json!({ "op": "identity_enable" })).await;
    let ember_id = identity["ember_id"].as_str().unwrap().to_owned();
    let (_, _, _, inspected) = worker
        .ask(json!({ "op": "bridge_inspect", "origin": origin }))
        .await;
    let bridge_id = inspected["profile"]["bridge_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (ok, reason, _, _) = worker
        .ask(json!({ "op": "bridge_approve", "origin": origin, "bridge_id": bridge_id }))
        .await;
    assert!(ok, "{reason:?}");
    let intent = provider_call(
        origin,
        provider,
        "/v1/link-intents",
        json!({ "subject": subject }),
    )
    .await;
    let (ok, reason, _, claim) = worker
        .ask(json!({ "op": "link_claim", "bridge_id": bridge_id, "connection_id": "mock-local", "code": intent["code"] }))
        .await;
    assert!(ok, "{reason:?}");
    let link = provider_call(
        origin,
        provider,
        &format!(
            "/v1/link-intents/{}/approve",
            intent["intent_id"].as_str().unwrap()
        ),
        json!({ "claim_id": claim["claim_id"], "ember_id": ember_id, "subject": subject }),
    )
    .await;
    (worker, ember_id, bridge_id, link)
}

#[tokio::test]
async fn play_a_set_through_two_helpers() {
    let dirs = Dirs(std::env::temp_dir().join(format!(
        "sf4-tournament-play-{}",
        ember_protocol::encoding::b64u(&random16())
    )));
    std::fs::create_dir_all(&dirs.0).unwrap();
    let (running, origin) = bridge(&dirs.0).await;
    let provider = ember_bridge::issue_credential(&running.state, Some("mock-local"), None, "play")
        .await
        .unwrap();
    let (host_endpoint, guest_endpoint) = ("a".repeat(64), "b".repeat(64));
    let host_dirs = Dirs(dirs.0.join("host"));
    let guest_dirs = Dirs(dirs.0.join("guest"));
    let (mut host, host_id, bridge_id, host_link) =
        linked_worker(&host_dirs, &host_endpoint, &origin, &provider, "host").await;
    let (mut guest, guest_id, _, guest_link) =
        linked_worker(&guest_dirs, &guest_endpoint, &origin, &provider, "guest").await;
    let created = provider_call(
        &origin,
        &provider,
        "/v1/matches",
        json!({
            "external_match_id": "helper-set",
            "game": "usf4",
            "participants": [
                { "participant_id": host_link["participant_id"], "ember_id": host_id, "slot": 0 },
                { "participant_id": guest_link["participant_id"], "ember_id": guest_id, "slot": 1 },
            ],
            "rules": {
                "games_to_win": 2, "draw_policy": "replay_no_score", "native_rules_profile": "ember-room-v1",
                "edition_policy": "ultra_only", "character_policy": "unrestricted_between_games",
                "stage_policy": "p1_selects", "input_delay_policy": "ember_existing_ready_policy",
            },
            "observer_policy": "authorized_only",
            "result_policy": "two_player_agreement_or_review",
            "required_build_id": "ember",
            "metadata": {},
        }),
    )
    .await;
    let match_id = created["match_id"].as_str().unwrap().to_owned();

    let (_, _, _, assignments) = host
        .ask(json!({ "op": "assignment_list", "bridge_id": bridge_id }))
        .await;
    assert_eq!(assignments["assignments"][0]["match_id"], match_id.as_str());
    // The site's Play button opens this page, which opens the match in Ember.
    assert_eq!(
        created["play_url"],
        format!("https://embernetplay.link/m#{bridge_id}/{match_id}").as_str()
    );
    // The first claim hosts; the second waits for the room.
    let claim = json!({ "op": "match_claim", "bridge_id": bridge_id, "match_id": match_id, "build": "build-1" });
    let (ok, reason, _, lease) = host.ask(claim.clone()).await;
    assert!(ok, "{reason:?}");
    assert_eq!(lease["role"], "host");
    let (_, _, _, wait) = guest.ask(claim.clone()).await;
    assert_eq!(wait["role"], "wait");
    let room_id = "0123456789abcdef0123456789abcdef";
    let (ok, reason, _, published) = host
        .ask(json!({ "op": "room_publish", "bridge_id": bridge_id, "match_id": match_id, "room": {
            "room_id": room_id, "invitation": "sf4e3:test", "lease_id": lease["lease_id"], "fence": lease["fence"], "replaces": null,
        }}))
        .await;
    assert!(ok, "{reason:?}");
    // The helper hands the game the checked binding, with its own slot.
    assert_eq!(published["binding"]["local_slot"], 0);
    let (_, _, _, joined) = guest.ask(claim.clone()).await;
    assert_eq!(joined["invitation"], "sf4e3:test");
    assert_eq!(joined["binding"]["local_slot"], 1);
    assert_eq!(
        joined["binding"]["fighters"][0]["endpoint_id"],
        host_endpoint.as_str()
    );

    for (generation, result) in [(3u64, "p1_win"), (4, "p1_win")] {
        let prepare = json!({ "op": "game_prepare", "bridge_id": bridge_id, "match_id": match_id, "match_generation": generation.to_string() });
        let (_, _, _, pending) = host.ask(prepare.clone()).await;
        assert_eq!(pending["state"], "pending");
        let (ok, reason, _, permitted) = guest.ask(prepare.clone()).await;
        assert!(ok, "{reason:?}");
        assert_eq!(permitted["state"], "permitted");
        let (_, _, _, again) = host.ask(prepare).await;
        assert_eq!(again["permit_id"], permitted["permit_id"]);
        let report = json!({
            "op": "game_report", "bridge_id": bridge_id, "match_id": match_id, "match_generation": generation.to_string(),
            "result": result, "capture_frame": "900", "confirmed_input_frame": "899",
        });
        let (ok, reason, _, first) = host.ask(report.clone()).await;
        assert!(ok, "{reason:?}");
        assert_eq!(
            (first["saved"].as_bool(), first["delivered"].as_bool()),
            (Some(true), Some(true))
        );
        let (_, _, _, second) = guest.ask(report).await;
        assert_eq!(second["attempt_state"], "accepted");
    }
    let (ok, _, _, last) = guest
        .ask(json!({ "op": "assignment_list", "bridge_id": bridge_id }))
        .await;
    assert!(ok);
    assert_eq!(last["assignments"][0]["state"], "completed");
    // Delivered reports leave nothing in the spool.
    let spooled = std::fs::read_dir(host_dirs.0.join("tournament").join("reports"))
        .map(|entries| entries.count())
        .unwrap_or(0);
    assert_eq!(spooled, 0);
    // A game this helper never got a permit for cannot be reported.
    let (ok, reason, _, _) = host
        .ask(json!({ "op": "game_report", "bridge_id": bridge_id, "match_id": match_id, "match_generation": "99", "result": "p1_win", "capture_frame": "1", "confirmed_input_frame": "1" }))
        .await;
    assert!(!ok);
    assert_eq!(reason.as_deref(), Some("unknown_permit"));
    running.abort();
}
