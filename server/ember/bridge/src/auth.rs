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
/// The web desk's session cookie (`routes::sign_in::desk`).
pub const DESK_COOKIE: &str = "ember_desk";
/// A web desk session ends this long after its sign-in, and sooner when
/// unused for `DESK_IDLE_SECS`: the desk changes nothing that needs the
/// game's proof, but it still acts for the organizer.
pub const DESK_SECS: u64 = 12 * 60 * 60;
pub const DESK_IDLE_SECS: u64 = 2 * 60 * 60;
/// How stale a desk session's last use may be before a request writes it
/// again, so a busy desk does not write on every read.
const DESK_TOUCH_SECS: u64 = 5 * 60;
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

/// An organizer signed in to the web desk: their Ember ID and the CSRF token
/// the desk's pages send back on every change.
pub struct Desk {
    pub ember_id: String,
    pub csrf: String,
    /// Opened from the game (`routes::sign_in::desk` handoff), which may
    /// make the changes a site sign-in alone may not.
    pub from_game: bool,
}

/// The web desk's session, if a valid cookie is present.
pub async fn desk(state: &AppState, headers: &HeaderMap) -> Result<Option<Desk>> {
    let Some(token) = cookie(headers, DESK_COOKIE) else {
        return Ok(None);
    };
    let token_hash = hash(&state.keys, "desk", &token);
    let csrf = csrf_token(&state.keys, &token);
    let now = state.now();
    let found: Option<(String, u64, bool)> = state
        .db
        .read(move |tx| {
            Ok(tx
                .query_row(
                    "SELECT ember_id, last_seen, from_game FROM desk_sessions
                     WHERE token_hash = ?1 AND expires_at > ?2 AND last_seen > ?3",
                    params![
                        token_hash.as_slice(),
                        now,
                        now.saturating_sub(DESK_IDLE_SECS)
                    ],
                    |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
                )
                .optional()?)
        })
        .await?;
    let Some((ember_id, last_seen, from_game)) = found else {
        return Ok(None);
    };
    if last_seen + DESK_TOUCH_SECS <= now {
        state
            .db
            .write(move |tx| {
                tx.execute(
                    "UPDATE desk_sessions SET last_seen = ?2 WHERE token_hash = ?1",
                    params![token_hash.as_slice(), now],
                )?;
                Ok(())
            })
            .await?;
    }
    Ok(Some(Desk {
        ember_id,
        csrf,
        from_game,
    }))
}

impl Desk {
    /// Whether a change carries the session's CSRF token in `X-Ember-CSRF`.
    pub fn csrf_ok(&self, headers: &HeaderMap) -> bool {
        headers
            .get("x-ember-csrf")
            .and_then(|value| value.to_str().ok())
            .is_some_and(|token| bool::from(token.as_bytes().ct_eq(self.csrf.as_bytes())))
    }
}

/// Starts a web desk session for `ember_id` and returns the cookie value,
/// which is stored only as a keyed hash.
/// `site` is the site it signed in with (empty when opened from the game)
/// and `agent` the browser's own short name, both kept so the game can list
/// the sessions.
pub fn start_desk_session(
    tx: &Transaction<'_>,
    keys: &Keys,
    ember_id: &str,
    site: &str,
    agent: &str,
    from_game: bool,
    now: u64,
) -> Result<String> {
    let token = new_token("emd");
    tx.execute(
        "DELETE FROM desk_sessions WHERE expires_at <= ?1 OR last_seen <= ?2",
        params![now, now.saturating_sub(DESK_IDLE_SECS)],
    )?;
    tx.execute(
        "INSERT INTO desk_sessions (token_hash, ember_id, expires_at, created_at, last_seen, site, agent, from_game)
         VALUES (?1, ?2, ?3, ?4, ?4, ?5, ?6, ?7)",
        params![
            hash(keys, "desk", &token).as_slice(),
            ember_id,
            now + DESK_SECS,
            now,
            site,
            agent,
            from_game
        ],
    )?;
    Ok(token)
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

/// Issues a provider or organizer credential, returning its id and its token.
/// The token is returned once and stored only as a keyed hash.
pub fn issue_credential(
    tx: &Transaction<'_>,
    keys: &Keys,
    tenant_id: &str,
    connection_id: Option<&str>,
    role: Role,
    label: &str,
    now: u64,
) -> Result<(String, String)> {
    let token = new_token(SERVICE_PREFIX);
    let id = new_id("cred");
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
            id,
            tenant_id,
            connection_id,
            role_name,
            label,
            now
        ],
    )?;
    Ok((id, token))
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
