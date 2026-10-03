//! A bridge on a real loopback listener, plus helpers that act as the Ember
//! helper (signed challenges), a provider backend, an organizer and a
//! browser. All keys and seeds here are throwaway test values.
#![allow(dead_code)]

pub mod league;

use std::path::PathBuf;

use ember_bridge::{AppState, Clock, Config, Db, Keys, Running, config};
use ember_protocol::{
    EmberId, SigningIdentity,
    challenge::{Action, Challenge, Expected, Method, ProvenRequest, command_digest},
    encoding::OriginPolicy,
    json::{self, Value},
};
use reqwest::{Client, Response, StatusCode};
use serde_json::{Value as Json, json};
use zeroize::Zeroizing;

pub struct Bridge {
    pub running: Option<Running>,
    pub origin: String,
    pub bridge_id: String,
    pub client: Client,
    pub clock: Clock,
    dir: PathBuf,
}

impl Drop for Bridge {
    fn drop(&mut self) {
        if let Some(running) = self.running.take() {
            running.abort();
        }
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

fn connection(id: &str) -> config::Connection {
    config::Connection {
        id: id.into(),
        kind: "mock".into(),
        environment: "local".into(),
        display_name: format!("Mock {id}"),
        enabled: true,
        api_base: None,
    }
}

impl Bridge {
    pub async fn start() -> Self {
        Self::start_with(|_| {}, Default::default()).await
    }

    /// A bridge whose configuration `adjust` changes first, with these
    /// integration secrets.
    pub async fn start_with(
        adjust: impl FnOnce(&mut Config),
        integrations: ember_bridge::integrations::Secrets,
    ) -> Self {
        let dir = std::env::temp_dir().join(format!(
            "ember-bridge-test-{}",
            ember_protocol::encoding::b64u(&rand16())
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let origin = format!("http://{}", listener.local_addr().unwrap());
        let bridge_id = ember_protocol::encoding::prefixed_id("brg", rand16());
        let mut config = Config {
            bridge_id: bridge_id.clone(),
            display_name: "Test bridge".into(),
            origin: origin.clone(),
            listen: "127.0.0.1:0".into(),
            database: dir.join("bridge.sqlite3"),
            secrets: dir.join("secrets.json"),
            allow_loopback_http: true,
            allow_private_webhooks: true,
            mock_browser: true,
            discord: None,
            integration_secrets: None,
            rooms: None,
            tenants: vec![
                config::Tenant {
                    id: "t1".into(),
                    name: "Tenant one".into(),
                    connections: vec![connection("mock-a"), connection("mock-b")],
                },
                config::Tenant {
                    id: "t2".into(),
                    name: "Tenant two".into(),
                    connections: vec![connection("mock-c")],
                },
            ],
        };
        adjust(&mut config);
        config.validate().unwrap();
        let db = Db::open(&config.database).unwrap();
        let clock = Clock::default();
        // Saved so `restart` can load the same keys, as a real restart does.
        let keys = Keys::generate();
        keys.save_new(&config.secrets).unwrap();
        let state = AppState::new(config, keys, integrations, db, clock.clone());
        ember_bridge::sync_config(&state).await.unwrap();
        let running = ember_bridge::start(state, listener).unwrap();
        Self {
            running: Some(running),
            origin,
            bridge_id,
            client: Client::builder()
                .redirect(reqwest::redirect::Policy::none())
                .build()
                .unwrap(),
            clock,
            dir,
        }
    }

    /// Stops the service and starts a new one over the same database, secrets
    /// file and configuration, on a new port, as a process restart would. The
    /// per-process state starts empty; sessions and rooms are as stored.
    pub async fn restart(&mut self, integrations: ember_bridge::integrations::Secrets) {
        let running = self.running.take().unwrap();
        let config = (*running.state.config).clone();
        running.abort();
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        self.origin = format!("http://{}", listener.local_addr().unwrap());
        let keys = Keys::load(&config.secrets).unwrap();
        let db = Db::open(&config.database).unwrap();
        let state = AppState::new(config, keys, integrations, db, self.clock.clone());
        self.running = Some(ember_bridge::start(state, listener).unwrap());
    }

    pub fn state(&self) -> &AppState {
        &self.running.as_ref().unwrap().state
    }

    pub fn url(&self, path: &str) -> String {
        format!("{}{path}", self.origin)
    }

    pub async fn provider(&self, connection: &str) -> String {
        ember_bridge::issue_credential(self.state(), Some(connection), None, "test provider")
            .await
            .unwrap()
    }

    pub async fn organizer(&self, tenant: &str) -> String {
        ember_bridge::issue_credential(self.state(), None, Some(tenant), "test organizer")
            .await
            .unwrap()
    }

    pub async fn get(&self, token: &str, path: &str) -> (StatusCode, Json) {
        read(
            self.client
                .get(self.url(path))
                .bearer_auth(token)
                .send()
                .await
                .unwrap(),
        )
        .await
    }

    pub async fn post(&self, token: &str, path: &str, body: Json) -> (StatusCode, Json) {
        self.post_keyed(token, path, body, None).await
    }

    pub async fn post_keyed(
        &self,
        token: &str,
        path: &str,
        body: Json,
        key: Option<&str>,
    ) -> (StatusCode, Json) {
        let mut request = self
            .client
            .post(self.url(path))
            .bearer_auth(token)
            .header("content-type", "application/json")
            .body(serde_json::to_vec(&body).unwrap());
        if let Some(key) = key {
            request = request.header("idempotency-key", key);
        }
        read(request.send().await.unwrap()).await
    }

    pub fn player(&self, byte: u8) -> Player {
        Player {
            identity: SigningIdentity::from_seed(&Zeroizing::new([byte; 32])).unwrap(),
            session: None,
        }
    }

    /// Requests a challenge for `command` at `action`/`path`, signs it as the
    /// helper would, and returns the proof-carrying body.
    pub async fn prove(
        &self,
        player: &Player,
        action: Action,
        method: Method,
        path: &str,
        command: Json,
    ) -> Vec<u8> {
        let command = json::to_value(&command).unwrap();
        let mut request = self
            .client
            .post(self.url("/v1/auth/challenges"))
            .header("content-type", "application/json")
            .body(
                serde_json::to_vec(&json!({
                    "public_key": player.identity.public_key(),
                    "action": action,
                    "method": method,
                    "path": path,
                    "request_digest": command_digest(&command),
                }))
                .unwrap(),
            );
        if let Some(session) = &player.session {
            request = request.bearer_auth(session);
        }
        let response = request.send().await.unwrap();
        assert_eq!(
            response.status(),
            StatusCode::CREATED,
            "challenge: {}",
            response.text().await.unwrap()
        );
        let challenge: Challenge = json::parse_as(&response.bytes().await.unwrap(), 8192).unwrap();
        let expected = Expected {
            bridge_id: &self.bridge_id,
            audience: &self.origin,
            ember_id: player.identity.ember_id(),
            action,
            method,
            path,
            command: &command,
        };
        let proof = challenge
            .sign(
                &player.identity,
                &expected,
                self.clock.now(),
                OriginPolicy::AllowLoopbackHttp,
            )
            .unwrap();
        ProvenRequest { command, proof }.to_json().unwrap()
    }

    pub async fn send_proof(
        &self,
        player: &Player,
        method: Method,
        path: &str,
        body: Vec<u8>,
    ) -> (StatusCode, Json) {
        let builder = match method {
            Method::Post => self.client.post(self.url(path)),
            Method::Delete => self.client.delete(self.url(path)),
        };
        let mut builder = builder
            .header("content-type", "application/json")
            .body(body);
        if let Some(session) = &player.session {
            builder = builder.bearer_auth(session);
        }
        read(builder.send().await.unwrap()).await
    }

    pub async fn open_session(&self, player: &mut Player) {
        let body = self
            .prove(
                player,
                Action::SessionCreate,
                Method::Post,
                "/v1/sessions",
                json!({ "requested_scopes": ["self:read", "tournament:participate"] }),
            )
            .await;
        let (status, created) = self
            .send_proof(player, Method::Post, "/v1/sessions", body)
            .await;
        assert_eq!(status, StatusCode::CREATED, "{created}");
        assert_eq!(created["ember_id"], player.id().as_str());
        player.session = Some(created["session_token"].as_str().unwrap().to_owned());
    }

    pub async fn claim(&self, player: &Player, code: &str, connection: &str) -> (StatusCode, Json) {
        let body = self
            .prove(
                player,
                Action::LinkClaim,
                Method::Post,
                "/v1/link-claims",
                json!({ "code": code, "connection_id": connection, "consent": true }),
            )
            .await;
        self.send_proof(player, Method::Post, "/v1/link-claims", body)
            .await
    }

    /// Provider-proxy linking of `subject` to `player` on `connection`.
    pub async fn link(
        &self,
        provider: &str,
        player: &Player,
        connection: &str,
        subject: &str,
    ) -> Json {
        let (status, intent) = self
            .post(
                provider,
                "/v1/link-intents",
                json!({ "subject": subject, "display_label": subject }),
            )
            .await;
        assert_eq!(status, StatusCode::CREATED, "{intent}");
        let (status, claim) = self
            .claim(player, intent["code"].as_str().unwrap(), connection)
            .await;
        assert_eq!(status, StatusCode::CREATED, "{claim}");
        let (status, link) = self
            .post(
                provider,
                &format!("/v1/link-intents/{}/approve", intent["intent_id"].as_str().unwrap()),
                json!({ "claim_id": claim["claim_id"], "ember_id": player.id(), "subject": subject }),
            )
            .await;
        assert_eq!(status, StatusCode::OK, "{link}");
        link
    }

    pub async fn events(&self, token: &str, after: &str) -> Vec<Json> {
        let (status, page) = self.get(token, &format!("/v1/events?after={after}")).await;
        assert_eq!(status, StatusCode::OK, "{page}");
        page["events"].as_array().unwrap().clone()
    }
}

pub struct Player {
    pub identity: SigningIdentity,
    pub session: Option<String>,
}

impl Player {
    pub fn id(&self) -> &EmberId {
        self.identity.ember_id()
    }

    pub fn token(&self) -> &str {
        self.session.as_deref().unwrap()
    }
}

pub async fn read(response: Response) -> (StatusCode, Json) {
    let status = response.status();
    let bytes = response.bytes().await.unwrap();
    let body = if bytes.is_empty() {
        Json::Null
    } else {
        serde_json::from_slice(&bytes)
            .unwrap_or_else(|_| Json::String(String::from_utf8_lossy(&bytes).into()))
    };
    (status, body)
}

pub fn code(body: &Json) -> &str {
    body["error"]["code"].as_str().unwrap_or("")
}

fn rand16() -> [u8; 16] {
    let mut bytes = [0; 16];
    getrandom::fill(&mut bytes).unwrap();
    bytes
}

pub fn value(json: Json) -> Value {
    json::to_value(&json).unwrap()
}

pub fn types(events: &[Json]) -> Vec<String> {
    events
        .iter()
        .map(|event| event["type"].as_str().unwrap().to_owned())
        .collect()
}
