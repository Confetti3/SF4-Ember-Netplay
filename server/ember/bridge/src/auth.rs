//! Authentication classes (spec 19.2). Each token kind has its own prefix
//! and hash purpose, so a player session never works as a provider
//! credential and the reverse (AUTH-06).
use axum::http::HeaderMap;
use ember_protocol::EmberId;
use rusqlite::{OptionalExtension, Transaction, params};
use subtle::ConstantTimeEq;

use crate::{
    AppState, Keys,
    error::{ApiFailure, Result},
    http::{bearer, cookie},
    util::{new_id, new_token},
};

pub const PLAYER_PREFIX: &str = "ems";
pub const SERVICE_PREFIX: &str = "emk";
pub const BROWSER_COOKIE: &str = "ember_mock_session";
pub const SESSION_SECS: u64 = 10 * 60;
pub const BROWSER_SECS: u64 = 60 * 60;
/// Replacing a link needs an external sign-in this recent (LINK-08).
pub const FRESH_AUTH_SECS: u64 = 5 * 60;

pub struct Player {
    pub ember_id: EmberId,
    pub scopes: Vec<String>,
    pub expires_at: u64,
    pub token_hash: [u8; 32],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Provider,
    Organizer,
}

#[derive(Clone, Debug)]
pub struct Service {
    pub id: String,
    pub tenant_id: String,
    /// Set for provider credentials; organizers act tenant-wide.
    pub connection_id: Option<String>,
    pub role: Role,
}

#[derive(Clone, Debug)]
pub struct Browser {
    pub token_hash: [u8; 32],
    pub connection_id: String,
    pub account_id: String,
    pub subject: String,
    pub authenticated_at: u64,
    /// HMAC of the session cookie: a synchronizer token the pages can render
    /// on every request without storing it.
    csrf: String,
}

pub enum Actor {
    Player(Player),
    Service(Service),
    Browser(Browser),
}

/// The credential a long-lived response was opened with, rechecked while it
/// runs so a revoked session or credential stops it.
#[derive(Clone)]
pub enum Standing {
    Player([u8; 32]),
    Service(String),
    Browser([u8; 32]),
}

impl Actor {
    pub fn standing(&self) -> Standing {
        match self {
            Self::Player(player) => Standing::Player(player.token_hash),
            Self::Service(service) => Standing::Service(service.id.clone()),
            Self::Browser(browser) => Standing::Browser(browser.token_hash),
        }
    }
}

/// Whether the credential still authorizes requests at `now`.
pub fn still_valid(
    tx: &rusqlite::Connection,
    standing: &Standing,
    now: u64,
) -> rusqlite::Result<bool> {
    match standing {
        Standing::Player(hash) => tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM sessions WHERE token_hash = ?1 AND revoked_at IS NULL AND expires_at > ?2)",
            params![hash.as_slice(), now],
            |row| row.get(0),
        ),
        Standing::Service(id) => tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM service_credentials WHERE id = ?1 AND revoked_at IS NULL)",
            [id],
            |row| row.get(0),
        ),
        Standing::Browser(hash) => tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM browser_sessions b JOIN provider_connections c ON c.id = b.connection_id
              WHERE b.token_hash = ?1 AND b.expires_at > ?2 AND c.enabled = 1)",
            params![hash.as_slice(), now],
            |row| row.get(0),
        ),
    }
}

pub fn hash(keys: &Keys, purpose: &str, token: &str) -> [u8; 32] {
    keys.keyed_hash(purpose, token.as_bytes())
}

pub async fn player(state: &AppState, headers: &HeaderMap) -> Result<Player> {
    let token = bearer(headers).ok_or_else(ApiFailure::unauthenticated)?;
    if !token.starts_with("ems_") {
        return Err(if token.starts_with("emk_") {
            ApiFailure::forbidden()
        } else {
            ApiFailure::unauthenticated()
        });
    }
    let token_hash = hash(&state.keys, "session", token);
    let now = state.now();
    state
        .db
        .read(move |tx| {
            let row = tx
                .query_row(
                    "SELECT ember_id, scopes, expires_at FROM sessions
                     WHERE token_hash = ?1 AND revoked_at IS NULL AND expires_at > ?2",
                    params![token_hash.as_slice(), now],
                    |row| {
                        Ok((
                            row.get::<_, String>(0)?,
                            row.get::<_, String>(1)?,
                            row.get::<_, u64>(2)?,
                        ))
                    },
                )
                .optional()?
                .ok_or_else(ApiFailure::unauthenticated)?;
            Ok(Player {
                ember_id: EmberId::parse(&row.0).map_err(|_| ApiFailure::unavailable())?,
                scopes: row.1.split(' ').map(str::to_owned).collect(),
                expires_at: row.2,
                token_hash,
            })
        })
        .await
}

pub async fn service(state: &AppState, headers: &HeaderMap) -> Result<Service> {
    let token = bearer(headers).ok_or_else(ApiFailure::unauthenticated)?;
    if !token.starts_with("emk_") {
        return Err(if token.starts_with("ems_") {
            ApiFailure::forbidden()
        } else {
            ApiFailure::unauthenticated()
        });
    }
    let token_hash = hash(&state.keys, "service", token);
    let service = state
        .db
        .read(move |tx| {
            tx.query_row(
                "SELECT id, tenant_id, connection_id, role FROM service_credentials
                 WHERE token_hash = ?1 AND revoked_at IS NULL",
                [token_hash.as_slice()],
                |row| {
                    Ok(Service {
                        id: row.get(0)?,
                        tenant_id: row.get(1)?,
                        connection_id: row.get(2)?,
                        role: if row.get::<_, String>(3)? == "provider" {
                            Role::Provider
                        } else {
                            Role::Organizer
                        },
                    })
                },
            )
            .optional()?
            .ok_or_else(ApiFailure::unauthenticated)
        })
        .await?;
    if let Some(connection) = &service.connection_id
        && !state
            .config
            .connection(connection)
            .is_some_and(|(_, connection)| connection.enabled)
    {
        return Err(ApiFailure::forbidden());
    }
    Ok(service)
}

/// The mock provider's browser session, if a valid cookie is present.
pub async fn browser(state: &AppState, headers: &HeaderMap) -> Result<Option<Browser>> {
    let Some(token) = cookie(headers, BROWSER_COOKIE) else {
        return Ok(None);
    };
    let token_hash = hash(&state.keys, "browser", &token);
    let csrf = csrf_token(&state.keys, &token);
    let now = state.now();
    state
        .db
        .read(move |tx| {
            Ok(tx
                .query_row(
                    "SELECT b.connection_id, b.account_id, a.subject, b.authenticated_at
                     FROM browser_sessions b
                     JOIN external_accounts a ON a.id = b.account_id
                     JOIN provider_connections c ON c.id = b.connection_id
                     WHERE b.token_hash = ?1 AND b.expires_at > ?2 AND c.enabled = 1",
                    params![token_hash.as_slice(), now],
                    |row| {
                        Ok(Browser {
                            token_hash,
                            connection_id: row.get(0)?,
                            account_id: row.get(1)?,
                            subject: row.get(2)?,
                            authenticated_at: row.get(3)?,
                            csrf: csrf.clone(),
                        })
                    },
                )
                .optional()?)
        })
        .await
}

/// Any one of the three classes, chosen by the credential presented.
pub async fn any(state: &AppState, headers: &HeaderMap) -> Result<Actor> {
    match bearer(headers) {
        Some(token) if token.starts_with("ems_") => {
            Ok(Actor::Player(player(state, headers).await?))
        }
        Some(_) => Ok(Actor::Service(service(state, headers).await?)),
        None => browser(state, headers)
            .await?
            .map(Actor::Browser)
            .ok_or_else(ApiFailure::unauthenticated),
    }
}

fn csrf_token(keys: &Keys, cookie: &str) -> String {
    ember_protocol::encoding::b64u(&hash(keys, "csrf", cookie))
}

impl Browser {
    pub fn csrf(&self) -> &str {
        &self.csrf
    }

    pub fn csrf_ok(&self, presented: Option<&str>) -> bool {
        presented.is_some_and(|token| bool::from(token.as_bytes().ct_eq(self.csrf.as_bytes())))
    }
}

impl Service {
    pub fn require(&self, role: Role) -> Result<()> {
        if self.role == role {
            Ok(())
        } else {
            Err(ApiFailure::forbidden())
        }
    }

    pub fn provider_connection(&self) -> Result<&str> {
        match (self.role, &self.connection_id) {
            (Role::Provider, Some(connection)) => Ok(connection),
            _ => Err(ApiFailure::forbidden()),
        }
    }
}

/// Issues a provider or organizer credential. The token is returned once and
/// stored only as a keyed hash.
pub fn issue_credential(
    tx: &Transaction<'_>,
    keys: &Keys,
    tenant_id: &str,
    connection_id: Option<&str>,
    role: Role,
    label: &str,
    now: u64,
) -> Result<String> {
    let token = new_token(SERVICE_PREFIX);
    let role_name = match role {
        Role::Provider => "provider",
        Role::Organizer => "organizer",
    };
    if (role == Role::Provider) != connection_id.is_some() {
        return Err(ApiFailure::invalid(
            "Provider credentials need a connection; organizer credentials do not.",
        ));
    }
    tx.execute(
        "INSERT INTO service_credentials (token_hash, id, tenant_id, connection_id, role, label, created_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
        params![
            hash(keys, "service", &token).as_slice(),
            new_id("cred"),
            tenant_id,
            connection_id,
            role_name,
            label,
            now
        ],
    )?;
    Ok(token)
}

/// Starts a mock-provider browser session for `account_id` and returns the
/// cookie value, which is stored only as a keyed hash.
pub fn start_browser_session(
    tx: &Transaction<'_>,
    keys: &Keys,
    connection_id: &str,
    account_id: &str,
    now: u64,
) -> Result<String> {
    let token = new_token("emb");
    tx.execute(
        "INSERT INTO browser_sessions (token_hash, connection_id, account_id, authenticated_at, expires_at)
         VALUES (?1, ?2, ?3, ?4, ?5)",
        params![
            hash(keys, "browser", &token).as_slice(),
            connection_id,
            account_id,
            now,
            now + BROWSER_SECS
        ],
    )?;
    Ok(token)
}
