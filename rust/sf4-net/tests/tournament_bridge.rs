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
        tenants: vec![config::Tenant {
            id: "local".into(),
            name: "Local".into(),
            connections: vec![config::Connection {
                id: "mock-local".into(),
                kind: "mock".into(),
                environment: "local".into(),
                display_name: "Mock provider".into(),
                enabled: true,
            }],
        }],
    };
    let db = Db::open(&config.database).unwrap();
    let state = AppState::new(config, Keys::generate(), db, Clock::default());
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
        let (sender, events) = mpsc::channel(64);
        let (handle, task) = tournament::spawn_in(
            sender,
            dirs.0.join("identity"),
            dirs.0.join("tournament"),
            false,
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
