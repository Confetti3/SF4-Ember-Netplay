//! The Ember tournament bridge (EMBER-TB-001 sections 9 to 23).
//!
//! A provider-neutral HTTP service that links persistent Ember identities to
//! external accounts, holds logical matches and their accepted results, and
//! publishes signed events. It is not a gameplay relay. Every request body
//! goes through `ember_protocol::json`'s strict profile; there is no lenient
//! JSON extractor anywhere in this crate.
mod auth;
pub mod config;
mod db;
mod delivery;
mod error;
mod events;
mod http;
mod mock;
mod rate;
mod routes;
mod secrets;
mod util;

use std::{net::SocketAddr, sync::Arc};

use tokio::{net::TcpListener, sync::Notify, task::JoinHandle};

pub use config::Config;
pub use db::Db;
pub use secrets::Keys;
pub use util::Clock;

/// Shared state of one running bridge.
#[derive(Clone)]
pub struct AppState {
    pub config: Arc<Config>,
    pub keys: Arc<Keys>,
    pub db: Db,
    pub clock: Clock,
    rate: Arc<rate::Limiter>,
    /// Wakes event streams and the delivery worker after a commit.
    events: Arc<Notify>,
    delivery: Arc<Notify>,
    stream_signal: tokio::sync::watch::Sender<u64>,
}

impl AppState {
    pub fn new(config: Config, keys: Keys, db: Db, clock: Clock) -> Self {
        Self {
            config: Arc::new(config),
            keys: Arc::new(keys),
            db,
            clock,
            rate: Arc::new(rate::Limiter::default()),
            events: Arc::new(Notify::new()),
            delivery: Arc::new(Notify::new()),
            stream_signal: tokio::sync::watch::channel(0).0,
        }
    }

    pub fn now(&self) -> u64 {
        self.clock.now()
    }

    /// Call after a transaction that may have written events.
    fn committed(&self) {
        self.events.notify_waiters();
        self.delivery.notify_one();
        self.stream_signal
            .send_modify(|generation| *generation += 1);
    }
}

/// A bridge serving on a bound listener, with its background workers.
pub struct Running {
    pub address: SocketAddr,
    pub state: AppState,
    server: JoinHandle<()>,
    workers: Vec<JoinHandle<()>>,
}

impl Running {
    pub fn abort(self) {
        self.server.abort();
        for worker in self.workers {
            worker.abort();
        }
    }
}

/// Serves `state` on `listener` and starts maintenance and delivery.
pub fn start(state: AppState, listener: TcpListener) -> std::io::Result<Running> {
    install_crypto_provider();
    let address = listener.local_addr()?;
    let app = routes::router(state.clone());
    let server = tokio::spawn(async move {
        let _ = axum::serve(
            listener,
            app.into_make_service_with_connect_info::<SocketAddr>(),
        )
        .await;
    });
    let workers = vec![
        tokio::spawn(delivery::run(state.clone())),
        tokio::spawn(routes::maintenance(state.clone())),
    ];
    Ok(Running {
        address,
        state,
        server,
        workers,
    })
}

/// Records configured tenants and connections. Connections removed from the
/// configuration are disabled, never deleted, so their history stays.
pub async fn sync_config(state: &AppState) -> Result<(), error::ApiFailure> {
    let config = state.config.clone();
    state
        .db
        .write(move |tx| {
            tx.execute("UPDATE provider_connections SET enabled = 0", [])?;
            for tenant in &config.tenants {
                tx.execute(
                    "INSERT INTO tenants (id, name) VALUES (?1, ?2) ON CONFLICT (id) DO UPDATE SET name = excluded.name",
                    rusqlite::params![tenant.id, tenant.name],
                )?;
                for connection in &tenant.connections {
                    tx.execute(
                        "INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
                         VALUES (?1, ?2, ?3, ?4, ?5, ?6)
                         ON CONFLICT (id) DO UPDATE SET display_name = excluded.display_name, enabled = excluded.enabled",
                        rusqlite::params![
                            connection.id,
                            tenant.id,
                            connection.kind,
                            connection.environment,
                            connection.display_name,
                            connection.enabled
                        ],
                    )?;
                    let (kind, environment, owner): (String, String, String) = tx.query_row(
                        "SELECT kind, environment, tenant_id FROM provider_connections WHERE id = ?1",
                        [&connection.id],
                        |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
                    )?;
                    // A connection ID never changes provider, environment or
                    // tenant (spec 22, PROVIDER-05); configure a new one.
                    if kind != connection.kind || environment != connection.environment || owner != tenant.id {
                        return Err(error::ApiFailure::invalid("a connection id changed kind, environment or tenant"));
                    }
                }
            }
            Ok(())
        })
        .await
}

/// Issues a provider credential for `connection`, or an organizer
/// credential for `tenant`. The token is returned once.
pub async fn issue_credential(
    state: &AppState,
    connection: Option<&str>,
    tenant: Option<&str>,
    label: &str,
) -> Result<String, error::ApiFailure> {
    let (tenant_id, connection_id, role) = match (connection, tenant) {
        (Some(connection), None) => {
            let (tenant, _) = state
                .config
                .connection(connection)
                .ok_or_else(|| error::ApiFailure::invalid("unknown connection"))?;
            (
                tenant.id.clone(),
                Some(connection.to_owned()),
                auth::Role::Provider,
            )
        }
        (None, Some(tenant)) => {
            if !state.config.tenants.iter().any(|t| t.id == tenant) {
                return Err(error::ApiFailure::invalid("unknown tenant"));
            }
            (tenant.to_owned(), None, auth::Role::Organizer)
        }
        _ => return Err(error::ApiFailure::invalid("name a connection or a tenant")),
    };
    let keys = state.keys.clone();
    let label = label.to_owned();
    let now = state.now();
    state
        .db
        .write(move |tx| {
            auth::issue_credential(
                tx,
                &keys,
                &tenant_id,
                connection_id.as_deref(),
                role,
                &label,
                now,
            )
        })
        .await
}

/// rustls uses ring, the provider iroh already ships, rather than aws-lc.
pub fn install_crypto_provider() {
    let _ = rustls::crypto::ring::default_provider().install_default();
}
