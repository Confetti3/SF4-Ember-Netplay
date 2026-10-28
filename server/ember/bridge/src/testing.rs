//! What the tests of this workspace share to run a real bridge on loopback:
//! the configuration they agree on, a started bridge that cleans up after
//! itself, a bridge rebuilt over the same database, a stand-in room
//! supervisor and a wait that fails with a message.
//!
//! Compiled only with the `test-support` feature. The tests of this crate
//! enable it through a dev-dependency on the crate itself, and the other
//! crates of the workspace through theirs, so a bridge built for deployment
//! does not contain it.
use std::{
    future::Future,
    ops::Deref,
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

use tokio::net::TcpListener;

use crate::{AppState, Clock, Config, Db, Keys, Running, config, integrations::Secrets};

pub mod supervisor;

/// OS randomness for a test's names and IDs.
pub fn random16() -> [u8; 16] {
    let mut bytes = [0; 16];
    getrandom::fill(&mut bytes).unwrap();
    bytes
}

/// A new folder under the system's temporary folder, named `name` and a
/// random suffix. The caller removes it.
pub fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!(
        "{name}-{}",
        ember_protocol::encoding::b64u(&random16())
    ));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// A test's own folder, removed when it is dropped.
pub struct TempDir(pub PathBuf);

impl TempDir {
    pub fn new(name: &str) -> Self {
        Self(temp_dir(name))
    }
}

impl Deref for TempDir {
    type Target = Path;

    fn deref(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// A connection of the mock kind, enabled.
pub fn connection(id: &str, display_name: &str) -> config::Connection {
    config::Connection {
        id: id.into(),
        kind: "mock".into(),
        environment: "local".into(),
        display_name: display_name.into(),
        enabled: true,
        api_base: None,
        discord_lookup: false,
        lookup_sites: Vec::new(),
        disputes: None,
        results_url: None,
        rooms: None,
    }
}

/// The configuration of a bridge served from `origin`, with its files in
/// `dir`: loopback HTTP and webhooks allowed, one tenant with one mock
/// connection, and none of the optional services. A test changes what it
/// needs.
pub fn loopback_config(dir: &Path, origin: &str) -> Config {
    Config {
        bridge_id: ember_protocol::encoding::prefixed_id("brg", random16()),
        display_name: "Loopback bridge".into(),
        origin: origin.to_owned(),
        listen: "127.0.0.1:0".into(),
        database: dir.join("bridge.sqlite3"),
        secrets: dir.join("secrets.json"),
        allow_loopback_http: true,
        allow_private_webhooks: true,
        mock_browser: false,
        discord: None,
        sign_in_sites: Vec::new(),
        desk: None,
        integration_secrets: None,
        rooms: None,
        match_expiry_hours: config::DEFAULT_MATCH_EXPIRY_HOURS,
        replay_retention: None,
        archive: None,
        trusted_proxies: Vec::new(),
        tenants: vec![config::Tenant {
            id: "local".into(),
            name: "Local".into(),
            connections: vec![connection("mock-local", "Mock provider")],
        }],
    }
}

/// Starts a bridge for `config` on `listener`, over a new database and new
/// keys, which are saved so that a restart can load them. `config.origin` is
/// where the bridge says it is served, which may be a stand-in in front of
/// `listener`.
pub async fn serve(
    config: Config,
    integrations: Secrets,
    clock: Clock,
    listener: TcpListener,
) -> Running {
    config.validate().unwrap();
    let db = Db::open(&config.database).unwrap();
    let keys = Keys::generate();
    keys.save_new(&config.secrets).unwrap();
    let state = AppState::new(config, keys, integrations, db, clock);
    crate::sync_config(&state).await.unwrap();
    crate::start(state, listener).unwrap()
}

/// A bridge over the same database as `state`, with the configuration and
/// the secrets of an operator's edit. It is not served and it has not
/// synced its configuration.
pub fn reconfigured(
    state: &AppState,
    edit: impl FnOnce(&mut Config),
    integrations: Secrets,
) -> AppState {
    let mut config = (*state.config).clone();
    edit(&mut config);
    AppState::new(
        config,
        Keys::generate(),
        integrations,
        state.db.clone(),
        state.clock.clone(),
    )
}

/// A bridge serving on a loopback port of its own, in a folder of its own.
/// Both are removed and the bridge stopped when it is dropped.
pub struct Loopback {
    pub running: Option<Running>,
    pub origin: String,
    pub bridge_id: String,
    pub clock: Clock,
    _dir: TempDir,
}

impl Drop for Loopback {
    fn drop(&mut self) {
        if let Some(running) = self.running.take() {
            running.abort();
        }
    }
}

impl Loopback {
    /// A bridge for `loopback_config`, changed by `adjust`, with these
    /// integration secrets.
    pub async fn start_with(adjust: impl FnOnce(&mut Config), integrations: Secrets) -> Self {
        let dir = TempDir::new("ember-bridge-test");
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let origin = format!("http://{}", listener.local_addr().unwrap());
        let mut config = loopback_config(&dir, &origin);
        adjust(&mut config);
        let bridge_id = config.bridge_id.clone();
        let clock = Clock::default();
        let running = serve(config, integrations, clock.clone(), listener).await;
        Self {
            running: Some(running),
            origin,
            bridge_id,
            clock,
            _dir: dir,
        }
    }

    /// Stops the service and starts a new one over the same database, secrets
    /// file and configuration, on a new port, as a process restart would. The
    /// per-process state starts empty; sessions and rooms are as stored. The
    /// bridge's origin is the new port's, so proofs made after it hold. It
    /// returns once the new one serves, so it has recorded its restart.
    pub async fn restart_with(&mut self, adjust: impl FnOnce(&mut Config), integrations: Secrets) {
        let running = self.running.take().unwrap();
        let mut config = (*running.state.config).clone();
        running.abort();
        adjust(&mut config);
        config.validate().unwrap();
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        self.origin = format!("http://{}", listener.local_addr().unwrap());
        config.origin = self.origin.clone();
        let keys = Keys::load(&config.secrets).unwrap();
        let db = Db::open(&config.database).unwrap();
        let state = AppState::new(config, keys, integrations, db, self.clock.clone());
        self.running = Some(crate::start(state, listener).unwrap());
        let served = reqwest::Client::new()
            .get(self.url("/.well-known/ember-bridge.json"))
            .send()
            .await
            .unwrap();
        assert_eq!(served.status(), reqwest::StatusCode::OK);
    }

    pub fn state(&self) -> &AppState {
        &self.running.as_ref().unwrap().state
    }

    pub fn url(&self, path: &str) -> String {
        format!("{}{path}", self.origin)
    }
}

/// How often a wait looks again.
const POLL: Duration = Duration::from_millis(20);

/// Waits until `ready` holds, looking every 20 ms, and panics naming `what`
/// when `limit` has passed. For something that is to become true; a test
/// that something does not happen waits a window of its own.
pub async fn until(what: &str, limit: Duration, ready: impl FnMut() -> bool) {
    assert!(
        polled(limit, ready).await,
        "gave up waiting for {what} after {limit:?}"
    );
}

/// `until`, for a condition that has to await to look.
pub async fn until_async<F, Fut>(what: &str, limit: Duration, ready: F)
where
    F: FnMut() -> Fut,
    Fut: Future<Output = bool>,
{
    assert!(
        polled_async(limit, ready).await,
        "gave up waiting for {what} after {limit:?}"
    );
}

/// Whether `ready` held within `limit`, looking every 20 ms, for a test that
/// has more to say than a name when it did not.
pub async fn polled(limit: Duration, mut ready: impl FnMut() -> bool) -> bool {
    polled_async(limit, || std::future::ready(ready())).await
}

/// `polled`, for a condition that has to await to look.
pub async fn polled_async<F, Fut>(limit: Duration, mut ready: F) -> bool
where
    F: FnMut() -> Fut,
    Fut: Future<Output = bool>,
{
    let started = Instant::now();
    while !ready().await {
        if started.elapsed() >= limit {
            return false;
        }
        tokio::time::sleep(POLL).await;
    }
    true
}
