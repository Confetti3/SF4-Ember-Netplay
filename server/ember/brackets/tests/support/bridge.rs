//! A real bridge on a loopback port, with the shipped Challonge and
//! start.gg sign-in site definitions and the connector's `direct`
//! connection: lookup by those sites' accounts and by Discord, disputes
//! restarted, results sent to the connector.
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

use axum::http::{Method, StatusCode};
use ember_bridge::{AppState, Db, Keys, Running};
use ember_protocol::{EmberId, SigningIdentity};
use rusqlite::params;
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
    /// A bridge whose `brackets` connection sends results to `results_url`,
    /// and looks players up by Discord account when `discord_lookup`.
    pub async fn start(results_url: &str, discord_lookup: bool) -> Self {
        let dir = temp("bridge");
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let origin = format!("http://{}", listener.local_addr().unwrap());
        // Built from JSON, as bridge.json is, so new optional settings need
        // no change here. The sites are the shipped definitions.
        let site = |name: &str| -> Value {
            let path = format!(
                "{}/../bridge/deploy/sites/{name}.json",
                env!("CARGO_MANIFEST_DIR")
            );
            serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap()
        };
        let config: ember_bridge::Config = serde_json::from_value(json!({
            "bridge_id": ember_protocol::encoding::prefixed_id("brg", rand16()),
            "display_name": "Test bridge",
            "origin": origin,
            "listen": "127.0.0.1:0",
            "database": dir.join("bridge.sqlite3"),
            "secrets": dir.join("secrets.json"),
            "allow_loopback_http": true,
            "allow_private_webhooks": true,
            "integration_secrets": dir.join("integrations.json"),
            "sign_in_sites": [site("challonge"), site("startgg")],
            "desk": { "connection": CONNECTION },
            "tenants": [{
                "id": "organizers",
                "name": "Organizers",
                "connections": [{
                    "id": CONNECTION,
                    "kind": "direct",
                    "environment": "local",
                    "display_name": "Brackets",
                    "lookup_sites": ["challonge", "startgg"],
                    "discord_lookup": discord_lookup,
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

    /// A player with Ember, seeded by `byte`, written as a session would.
    async fn player(&self, byte: u8) -> EmberId {
        let identity = SigningIdentity::from_seed(&Zeroizing::new([byte; 32])).unwrap();
        let ember_id = identity.ember_id().clone();
        let row = [
            ember_id.as_str().to_owned(),
            identity.public_key().to_b64u(),
        ];
        self.state()
            .db
            .write(move |tx| {
                tx.execute(
                    "INSERT OR IGNORE INTO identities (ember_id, public_key, created_at) VALUES (?1, ?2, 1)",
                    params![row[0], row[1]],
                )?;
                Ok(())
            })
            .await
            .unwrap();
        ember_id
    }

    /// A player who connected an account on a sign-in site, written as the
    /// sign-in would (the bridge's own tests cover it). `username_key` is
    /// kept only for a site that looks accounts up by username.
    async fn connect_site(
        &self,
        byte: u8,
        site: &'static str,
        user_id: String,
        username: String,
        by_username: bool,
    ) -> EmberId {
        let ember_id = self.player(byte).await;
        let id = ember_id.as_str().to_owned();
        // The shipped Challonge definition compares usernames without case.
        let key = by_username.then(|| username.to_ascii_lowercase());
        self.state()
            .db
            .write(move |tx| {
                tx.execute(
                    "INSERT INTO site_accounts (site, user_id, ember_id, username, username_key, connected_at)
                     VALUES (?1, ?2, ?3, ?4, ?5, 1)",
                    params![site, user_id, id, username, key],
                )?;
                Ok(())
            })
            .await
            .unwrap();
        ember_id
    }

    /// A player with Ember who connected the Challonge account `username`.
    pub async fn connect(&self, byte: u8, username: &str) -> EmberId {
        let user_id = (9_000 + u32::from(byte)).to_string();
        self.connect_site(byte, "challonge", user_id, username.to_owned(), true)
            .await
    }

    /// A player with Ember who connected the start.gg account `user_id`.
    pub async fn connect_startgg(&self, byte: u8, user_id: u64) -> EmberId {
        let gamer_tag = format!("Player {byte}");
        self.connect_site(byte, "startgg", user_id.to_string(), gamer_tag, false)
            .await
    }

    /// A player with Ember who connected the Discord account `user_id`, as
    /// the bridge's partner tests write it.
    pub async fn connect_discord(&self, byte: u8, user_id: &str) -> EmberId {
        let ember_id = self.player(byte).await;
        let row = [user_id.to_owned(), ember_id.as_str().to_owned()];
        self.state()
            .db
            .write(move |tx| {
                tx.execute(
                    "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, 'name', 1)",
                    params![row[0], row[1]],
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
