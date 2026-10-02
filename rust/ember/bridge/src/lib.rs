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
pub mod integrations;
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
    pub integrations: Arc<integrations::Secrets>,
    pub db: Db,
    pub clock: Clock,
    rate: Arc<rate::Limiter>,
    /// Wakes event streams and the delivery worker after a commit.
    events: Arc<Notify>,
    delivery: Arc<Notify>,
    stream_signal: tokio::sync::watch::Sender<u64>,
}

impl AppState {
    pub fn new(
        config: Config,
        keys: Keys,
        integrations: integrations::Secrets,
        db: Db,
        clock: Clock,
    ) -> Self {
        Self {
            config: Arc::new(config),
            keys: Arc::new(keys),
            integrations: Arc::new(integrations),
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
        tokio::spawn(delivery::run(state.clone(), delivery::webhooks::Webhooks)),
        tokio::spawn(routes::maintenance(state.clone())),
        tokio::spawn(delivery::run(
            state.clone(),
            routes::blumint::Results::new(&state),
        )),
    ];
    Ok(Running {
        address,
        state,
        server,
        workers,
    })
}

/// One pass of the bridge's periodic work: expiring link intents, holding
/// games whose reports did not arrive, and pruning stale records. `start`
/// runs it every few seconds; tests call it after moving the clock.
pub async fn maintain(state: &AppState) {
    routes::maintenance_once(state).await;
}

/// One pass of sending finished matches' results to BluMint; tests call it.
pub async fn deliver_to_blumint(state: &AppState) {
    delivery::pass(state, &Arc::new(routes::blumint::Results::new(state))).await;
}

/// Tells BluMint where the connection's endpoints are (`routes::blumint`).
pub async fn register_blumint(state: &AppState, connection_id: &str) -> Result<(), String> {
    routes::blumint::register(state, connection_id).await
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

/// One issued service credential, without its token.
#[derive(Debug)]
pub struct CredentialInfo {
    pub id: String,
    pub tenant_id: String,
    pub connection_id: Option<String>,
    pub role: String,
    pub label: String,
    pub created_at: u64,
    pub revoked_at: Option<u64>,
}

/// Every service credential ever issued, oldest first.
pub async fn list_credentials(state: &AppState) -> Result<Vec<CredentialInfo>, error::ApiFailure> {
    state
        .db
        .read(|tx| {
            let mut statement = tx.prepare(
                "SELECT id, tenant_id, connection_id, role, label, created_at, revoked_at
                   FROM service_credentials ORDER BY created_at, id",
            )?;
            let rows = statement.query_map([], |row| {
                Ok(CredentialInfo {
                    id: row.get(0)?,
                    tenant_id: row.get(1)?,
                    connection_id: row.get(2)?,
                    role: row.get(3)?,
                    label: row.get(4)?,
                    created_at: row.get(5)?,
                    revoked_at: row.get(6)?,
                })
            })?;
            Ok(rows.collect::<Result<Vec<_>, _>>()?)
        })
        .await
}

/// Revokes the credential `id`. Requests with it fail from the next one on,
/// event streams it opened end at their next check, and the webhook
/// subscriptions it created are disabled with their secrets wiped and their
/// pending deliveries cancelled, as a delete would. `Ok(false)` when it was
/// already revoked.
pub async fn revoke_credential(state: &AppState, id: &str) -> Result<bool, error::ApiFailure> {
    use rusqlite::OptionalExtension;
    let id = id.to_owned();
    let now = state.now();
    state
        .db
        .write(move |tx| {
            let revoked: Option<Option<u64>> = tx
                .query_row(
                    "SELECT revoked_at FROM service_credentials WHERE id = ?1",
                    [&id],
                    |row| row.get(0),
                )
                .optional()?;
            match revoked {
                None => Err(error::ApiFailure::invalid("unknown credential")),
                Some(Some(_)) => Ok(false),
                Some(None) => {
                    tx.execute(
                        "UPDATE service_credentials SET revoked_at = ?1 WHERE id = ?2",
                        rusqlite::params![now, id],
                    )?;
                    tx.execute(
                        "UPDATE delivery_outbox SET state = 'cancelled' WHERE state = 'pending'
                           AND subscription_id IN (SELECT id FROM webhook_subscriptions WHERE owner_credential = ?1)",
                        [&id],
                    )?;
                    tx.execute(
                        "UPDATE webhook_subscriptions SET enabled = 0, disabled_at = COALESCE(disabled_at, ?1),
                            secret_sealed = x'', previous_secret_sealed = NULL, previous_expires_at = NULL
                         WHERE owner_credential = ?2",
                        rusqlite::params![now, id],
                    )?;
                    Ok(true)
                }
            }
        })
        .await
}

/// rustls uses ring, the provider iroh already ships, rather than aws-lc.
pub fn install_crypto_provider() {
    let _ = rustls::crypto::ring::default_provider().install_default();
}
