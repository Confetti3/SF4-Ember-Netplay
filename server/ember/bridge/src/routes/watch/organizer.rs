//! The organizer's side of watching: whether anyone holding a match link may
//! watch, who is always approved, and who is denied one match. These routes
//! only write rows; the next `rewatch` applies them to the binding. Provider
//! credentials of the connection may read; connections whose kind fixes their
//! settings (BluMint) refuse every write.
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Response},
};
use ember_protocol::{EmberId, api::ErrorCode};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Deserialize;
use serde_json::json;

use super::{
    Nothing,
    access::Policy,
    rewatch::{LINK_HOLDER_CAP, bound, rows},
};
use crate::{
    AppState,
    audit::audit,
    auth::{self, Service},
    config::BLUMINT,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    routes::ledger::load,
};

/// What a route's path names, and the connection that owns it.
enum Scope {
    Connection(String),
    Match(String),
}

struct Resolved {
    tenant_id: String,
    connection_id: String,
    /// The connection's or the match's ID: what an audit entry targets.
    id: String,
}

impl Scope {
    fn resolve(self, tx: &Transaction<'_>) -> Result<Resolved> {
        match self {
            Self::Connection(id) => Ok(Resolved {
                tenant_id: tx
                    .query_row(
                        "SELECT tenant_id FROM provider_connections WHERE id = ?1",
                        [&id],
                        |row| row.get(0),
                    )
                    .optional()?
                    .ok_or_else(ApiFailure::not_found)?,
                connection_id: id.clone(),
                id,
            }),
            Self::Match(id) => {
                let found = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
                Ok(Resolved {
                    tenant_id: found.tenant_id,
                    connection_id: found.connection_id,
                    id,
                })
            }
        }
    }
}

/// Runs `work` for the credential's standing to `scope`: any matching
/// credential reads it, only an organizer of its tenant writes, and a
/// connection fixed by its kind is never written.
async fn scoped<T: Send + 'static>(
    state: &AppState,
    headers: &HeaderMap,
    scope: Scope,
    write: bool,
    work: impl FnOnce(&Transaction<'_>, &Ctx, &Service, &Resolved) -> Result<T> + Send + 'static,
) -> Result<T> {
    let service = auth::service(state, headers).await?;
    let ctx = Ctx::of(state);
    let run = move |tx: &Transaction<'_>| {
        let resolved = scope.resolve(tx)?;
        let access = service.access_to(&resolved.tenant_id, &resolved.connection_id)?;
        if write {
            access.manage()?;
            let kind: String = tx.query_row(
                "SELECT kind FROM provider_connections WHERE id = ?1",
                [&resolved.connection_id],
                |row| row.get(0),
            )?;
            if kind == BLUMINT {
                return Err(ApiFailure::new(
                    ErrorCode::Forbidden,
                    "This connection's platform fixes who may watch its matches.",
                )
                .detail("reason", "connection_fixed"));
            }
        }
        work(tx, &ctx, &service, &resolved)
    };
    if write {
        state.db.write(run).await
    } else {
        state.db.read(run).await
    }
}

fn ember_id(text: &str) -> Result<EmberId> {
    EmberId::parse(text).map_err(|_| ApiFailure::invalid("That is not an Ember ID."))
}

fn written(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    by: &Service,
    action: &str,
    at: &Resolved,
) -> Result<()> {
    audit(
        tx,
        ctx.now,
        ("organizer", by.id.clone()),
        action,
        &at.id,
        "ok",
        None,
    )
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct LinkHolders {
    link_holders: bool,
}

fn policy_body(tx: &Transaction<'_>, connection_id: &str) -> Result<serde_json::Value> {
    let link_holders: bool = tx.query_row(
        "SELECT watch_link_holders FROM provider_connections WHERE id = ?1",
        [connection_id],
        |row| row.get(0),
    )?;
    let staff = tx
        .prepare(
            "SELECT ember_id, added_at FROM connection_watch_staff WHERE connection_id = ?1 ORDER BY added_at, ember_id",
        )?
        .query_map([connection_id], |row| {
            Ok(json!({ "ember_id": row.get::<_, String>(0)?, "added_at": row.get::<_, u64>(1)? }))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    Ok(json!({
        "connection_id": connection_id,
        "link_holders": link_holders,
        "link_holder_cap": LINK_HOLDER_CAP,
        "ceiling": ember_protocol::play::MAX_WATCHERS,
        "staff": staff,
    }))
}

/// `GET /v1/connections/{id}/watch`.
pub async fn connection_policy(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(connection_id): Path<String>,
) -> Result<Response> {
    let body = scoped(
        &state,
        &headers,
        Scope::Connection(connection_id),
        false,
        |tx, _, _, at| policy_body(tx, &at.connection_id),
    )
    .await?;
    Ok(ok(&body))
}

/// `PUT /v1/connections/{id}/watch`: whether anyone holding a match link may
/// watch. That puts a fighter's network address within reach of strangers, so
/// it is the organizer's explicit choice, off by default.
pub async fn set_link_holders(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(connection_id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let command: LinkHolders = body.parse()?;
    let body = scoped(
        &state,
        &headers,
        Scope::Connection(connection_id),
        true,
        move |tx, ctx, by, at| {
            tx.execute(
                "UPDATE provider_connections SET watch_link_holders = ?1 WHERE id = ?2",
                params![command.link_holders, at.connection_id],
            )?;
            written(tx, ctx, by, "watch.link_holders.set", at)?;
            policy_body(tx, &at.connection_id)
        },
    )
    .await?;
    Ok(ok(&body))
}

/// `PUT /v1/connections/{id}/watch/staff/{ember_id}`.
pub async fn set_staff(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((connection_id, who)): Path<(String, String)>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    body.parse::<Nothing>()?;
    let who = ember_id(&who)?;
    let body = scoped(
        &state,
        &headers,
        Scope::Connection(connection_id),
        true,
        move |tx, ctx, by, at| {
            tx.execute(
                "INSERT OR IGNORE INTO connection_watch_staff (connection_id, ember_id, added_at, added_by)
                 VALUES (?1, ?2, ?3, ?4)",
                params![at.connection_id, who.as_str(), ctx.now, by.id],
            )?;
            written(tx, ctx, by, "watch.staff.add", at)?;
            Ok(tx.query_row(
                "SELECT added_at FROM connection_watch_staff WHERE connection_id = ?1 AND ember_id = ?2",
                params![at.connection_id, who.as_str()],
                |row| Ok(json!({ "ember_id": who, "added_at": row.get::<_, u64>(0)? })),
            )?)
        },
    )
    .await?;
    Ok(ok(&body))
}

/// `DELETE /v1/connections/{id}/watch/staff/{ember_id}`.
pub async fn remove_staff(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((connection_id, who)): Path<(String, String)>,
) -> Result<Response> {
    let who = ember_id(&who)?;
    scoped(
        &state,
        &headers,
        Scope::Connection(connection_id),
        true,
        move |tx, ctx, by, at| {
            tx.execute(
                "DELETE FROM connection_watch_staff WHERE connection_id = ?1 AND ember_id = ?2",
                params![at.connection_id, who.as_str()],
            )?;
            written(tx, ctx, by, "watch.staff.remove", at)
        },
    )
    .await?;
    Ok(StatusCode::NO_CONTENT.into_response())
}

/// `PUT /v1/matches/{id}/watch/denials/{ember_id}`: the watcher is out of this
/// match, staff or not.
pub async fn deny(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((match_id, who)): Path<(String, String)>,
) -> Result<Response> {
    let who = ember_id(&who)?;
    let body = scoped(
        &state,
        &headers,
        Scope::Match(match_id),
        true,
        move |tx, ctx, by, at| {
            tx.execute(
                "INSERT OR IGNORE INTO match_watch_denials (match_id, ember_id, set_at, set_by)
                 VALUES (?1, ?2, ?3, ?4)",
                params![at.id, who.as_str(), ctx.now, by.id],
            )?;
            written(tx, ctx, by, "watch.deny.set", at)?;
            Ok(tx.query_row(
                "SELECT set_at FROM match_watch_denials WHERE match_id = ?1 AND ember_id = ?2",
                params![at.id, who.as_str()],
                |row| Ok(json!({ "ember_id": who, "set_at": row.get::<_, u64>(0)? })),
            )?)
        },
    )
    .await?;
    Ok(ok(&body))
}

/// `DELETE /v1/matches/{id}/watch/denials/{ember_id}`.
pub async fn clear_denial(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((match_id, who)): Path<(String, String)>,
) -> Result<Response> {
    let who = ember_id(&who)?;
    scoped(
        &state,
        &headers,
        Scope::Match(match_id),
        true,
        move |tx, ctx, by, at| {
            tx.execute(
                "DELETE FROM match_watch_denials WHERE match_id = ?1 AND ember_id = ?2",
                params![at.id, who.as_str()],
            )?;
            written(tx, ctx, by, "watch.deny.clear", at)
        },
    )
    .await?;
    Ok(StatusCode::NO_CONTENT.into_response())
}

/// `GET /v1/matches/{id}/watchers`: everyone who asked to watch this match,
/// whether the policy lets them, and whether the binding names them.
pub async fn match_watchers(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(match_id): Path<String>,
) -> Result<Response> {
    let body = scoped(
        &state,
        &headers,
        Scope::Match(match_id),
        false,
        |tx, _, _, at| {
            let policy = Policy::load(tx, &at.connection_id, &at.id)?;
            let named = bound(
                tx.query_row(
                    "SELECT bound_watchers FROM match_rooms WHERE match_id = ?1",
                    [&at.id],
                    |row| row.get::<_, Option<String>>(0),
                )
                .optional()?
                .flatten()
                .as_deref(),
            )?;
            let watchers: Vec<_> = rows(tx, &at.id, -1)?
                .into_iter()
                .map(|row| {
                    json!({
                        "ember_id": row.ember_id,
                        "via": policy.admission(&row.ember_id).via(),
                        "bound": named.iter().any(|w| w.ember_id == row.ember_id),
                        "first_seen_at": row.first_seen_at,
                        "last_seen_at": row.last_seen_at,
                    })
                })
                .collect();
            let denied: Vec<String> = tx
                .prepare("SELECT ember_id FROM match_watch_denials WHERE match_id = ?1 ORDER BY set_at, ember_id")?
                .query_map([&at.id], |row| row.get(0))?
                .collect::<rusqlite::Result<_>>()?;
            Ok(json!({
                "match_id": at.id,
                "link_holders": policy.link_holders(),
                "watchers": watchers,
                "denied": denied,
            }))
        },
    )
    .await?;
    Ok(ok(&body))
}
