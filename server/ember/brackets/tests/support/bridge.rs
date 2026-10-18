//! A real bridge on a loopback port, with the connector's `direct`
//! connection: Challonge lookup, disputes restarted, results sent to the
//! connector.
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

use axum::http::{Method, StatusCode};
use ember_bridge::{AppState, Db, Keys, Running};
use ember_protocol::{EmberId, SigningIdentity, challonge::username_key};
use serde_json::{Value, json};
use tokio::net::TcpListener;
use zeroize::Zeroizing;

use super::{CONNECTION, result_secret, temp};

pub struct TestBridge {
    running: Option<Running>,
    pub origin: String,
    pub clock: ember_bridge::Clock,
    pub http: reqwest::Client,
    dir: PathBuf,
}

impl Drop for TestBridge {
    fn drop(&mut self) {
        if let Some(running) = self.running.take() {
            running.abort();
        }
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

fn rand16() -> [u8; 16] {
    let mut bytes = [0; 16];
    getrandom::fill(&mut bytes).unwrap();
    bytes
}

impl TestBridge {
    /// A bridge whose `brackets` connection sends results to `results_url`.
    pub async fn start(results_url: &str) -> Self {
        let dir = temp("bridge");
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let origin = format!("http://{}", listener.local_addr().unwrap());
        // Built from JSON, as bridge.json is, so new optional settings need
        // no change here.
        let config: ember_bridge::Config = serde_json::from_value(json!({
            "bridge_id": ember_protocol::encoding::prefixed_id("brg", rand16()),
            "display_name": "Test bridge",
            "origin": origin,
            "listen": "127.0.0.1:0",
            "database": dir.join("bridge.sqlite3"),
            "secrets": dir.join("secrets.json"),
            "allow_loopback_http": true,
            "allow_private_webhooks": true,
            "tenants": [{
                "id": "organizers",
                "name": "Organizers",
                "connections": [{
                    "id": CONNECTION,
                    "kind": "direct",
                    "environment": "local",
                    "display_name": "Brackets",
                    "challonge_lookup": true,
                    "disputes": "restart",
                    "results_url": results_url,
                }],
            }],
        }))
        .unwrap();
        config.validate().unwrap();
        let db = Db::open(&config.database).unwrap();
        let clock = ember_bridge::Clock::default();
        let integrations = ember_bridge::integrations::Secrets {
            result_secrets: [(CONNECTION.to_owned(), result_secret())].into(),
            ..Default::default()
        };
        let state = AppState::new(config, Keys::generate(), integrations, db, clock.clone());
        ember_bridge::sync_config(&state).await.unwrap();
        let running = ember_bridge::start(state, listener).unwrap();
        Self {
            running: Some(running),
            origin,
            clock,
            http: reqwest::Client::new(),
            dir,
        }
    }

    pub fn state(&self) -> &AppState {
        &self.running.as_ref().unwrap().state
    }

    pub async fn provider(&self) -> String {
        ember_bridge::issue_credential(self.state(), Some(CONNECTION), None, "brackets")
            .await
            .unwrap()
    }

    pub async fn organizer(&self) -> String {
        ember_bridge::issue_credential(self.state(), None, Some("organizers"), "organizer")
            .await
            .unwrap()
    }

    /// A player with Ember who connected the Challonge account `username`,
    /// written as the sign-in would (the bridge's own tests cover it).
    pub async fn connect(&self, byte: u8, username: &str) -> EmberId {
        let identity = SigningIdentity::from_seed(&Zeroizing::new([byte; 32])).unwrap();
        let ember_id = identity.ember_id().clone();
        let row = [
            ember_id.as_str().to_owned(),
            identity.public_key().to_b64u(),
            (9_000 + u32::from(byte)).to_string(),
            username.to_owned(),
            username_key(username),
        ];
        self.state()
            .db
            .write(move |tx| {
                let [id, key, user_id, username, username_key] = &row;
                tx.execute(
                    "INSERT INTO identities (ember_id, public_key, created_at) VALUES (?1, ?2, 1)",
                    [id, key],
                )?;
                tx.execute(
                    "INSERT INTO challonge_accounts (user_id, ember_id, username, username_key, connected_at)
                     VALUES (?1, ?2, ?3, ?4, 1)",
                    [user_id, id, username, username_key],
                )?;
                Ok(())
            })
            .await
            .unwrap();
        ember_id
    }

    pub async fn send(
        &self,
        token: &str,
        method: Method,
        path: &str,
        body: Option<Value>,
        key: Option<&str>,
    ) -> (StatusCode, Value) {
        let mut request = self
            .http
            .request(method, format!("{}{path}", self.origin))
            .bearer_auth(token);
        if let Some(body) = body {
            request = request.json(&body);
        }
        if let Some(key) = key {
            request = request.header("idempotency-key", key);
        }
        let response = request.send().await.unwrap();
        let status = response.status();
        (status, response.json().await.unwrap_or(Value::Null))
    }

    /// Every match on the bridge: (external ID, match ID, state), oldest
    /// first.
    pub async fn matches(&self) -> Vec<(String, String, String)> {
        self.state()
            .db
            .read(|tx| {
                let mut statement = tx.prepare(
                    "SELECT external_match_id, id, state FROM matches ORDER BY created_at, rowid",
                )?;
                let rows = statement
                    .query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))?
                    .collect::<rusqlite::Result<Vec<_>>>()?;
                Ok(rows)
            })
            .await
            .unwrap()
    }

    /// Runs delivery passes, beside the bridge's own worker, until the SQL
    /// condition `settled` holds, for at most `DELIVERY_DEADLINE`. Either
    /// may be the one that sends; waiting on the recorded outcome rather
    /// than on either keeps a test independent of how fast the worker runs.
    async fn deliver_until(&self, what: &str, settled: &'static str, params: Vec<String>) {
        let deadline = Instant::now() + DELIVERY_DEADLINE;
        while Instant::now() < deadline {
            let params = params.clone();
            let done: bool = self
                .state()
                .db
                .read(move |tx| {
                    Ok(
                        tx.query_row(settled, rusqlite::params_from_iter(params), |row| {
                            row.get(0)
                        })?,
                    )
                })
                .await
                .unwrap();
            if done {
                return;
            }
            ember_bridge::deliver_results(self.state()).await;
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
        panic!("{what} within {DELIVERY_DEADLINE:?}");
    }

    /// Tries to deliver the match's result until the bridge records that it
    /// did not get through (`retrying` or `failed`), as it does when nothing
    /// answers on the results URL.
    pub async fn fail_delivery(&self, match_id: &str) {
        self.deliver_until(
            "the delivery was not recorded as failing",
            "SELECT delivery_state IN ('retrying', 'failed') FROM matches WHERE id = ?1",
            vec![match_id.to_owned()],
        )
        .await;
    }

    /// Sends the results of finished matches, until every one has been
    /// received.
    pub async fn deliver(&self) {
        self.deliver_until(
            "results were not delivered",
            "SELECT NOT EXISTS (SELECT 1 FROM matches
                 WHERE state IN ('completed', 'cancelled', 'expired', 'failed')
                   AND delivery_state <> 'delivered')",
            Vec::new(),
        )
        .await;
    }
}

/// How long a test waits for the bridge's deliveries to settle. Generous:
/// the deadline only matters when something is wrong.
const DELIVERY_DEADLINE: Duration = Duration::from_secs(30);
