//! Replayed answers for retried requests (spec 10.2, 19.1): a write route
//! that takes an `Idempotency-Key` stores its answer under the key, scoped to
//! the caller, and answers a repeat with it.
use axum::{http::StatusCode, response::Response};
use ember_protocol::{api::ErrorCode, json};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Serialize;

use crate::{
    AppState,
    ctx::Ctx,
    error::{ApiFailure, Result},
};

/// Replays a stored response for a retried request, or records a new one
/// (spec 10.2, 19.1). The key is scoped to the authenticated actor. A `work`
/// that fails stores nothing, so the same key can try again; its error is
/// the route's own (`E`), as long as the bridge's failures convert into it.
/// For a route that changes the answer before sending it; the others answer
/// in the stored bytes (`ServiceCall`).
pub fn idempotent<E: From<ApiFailure>>(
    tx: &Transaction<'_>,
    actor: &str,
    path: &str,
    key: &str,
    digest: &str,
    now: u64,
    work: impl FnOnce(&Transaction<'_>) -> std::result::Result<(StatusCode, serde_json::Value), E>,
) -> std::result::Result<(StatusCode, serde_json::Value), E> {
    if let Some(found) = replay(tx, actor, path, key, digest)? {
        let body = serde_json::from_slice(&found.body).map_err(|_| ApiFailure::unavailable())?;
        return Ok((status_of(&found), body));
    }
    let (status, body) = work(tx)?;
    record(tx, actor, path, key, digest, status, &encode(&body)?, now)?;
    Ok((status, body))
}

/// `idempotent`, answering in bytes: a new answer is encoded once, and those
/// bytes are both stored and returned; a repeat gets the stored bytes as
/// they are. They are what encoding the answer again would make, since
/// every stored answer is serde_json's encoding of a JSON value, which is
/// the same text after a round trip. A stored answer that is not JSON
/// fails as it always has.
pub fn idempotent_bytes(
    tx: &Transaction<'_>,
    actor: &str,
    path: &str,
    key: &str,
    digest: &str,
    now: u64,
    work: impl FnOnce(&Transaction<'_>) -> Result<(StatusCode, serde_json::Value)>,
) -> Result<(StatusCode, Vec<u8>)> {
    if let Some(found) = replay(tx, actor, path, key, digest)? {
        serde_json::from_slice::<serde::de::IgnoredAny>(&found.body)
            .map_err(|_| ApiFailure::unavailable())?;
        return Ok((status_of(&found), found.body));
    }
    let (status, body) = work(tx)?;
    let body = encode(&body)?;
    record(tx, actor, path, key, digest, status, &body, now)?;
    Ok((status, body))
}

/// The answer stored under `key` for a repeat of the request `digest`
/// describes; a different request under the key is refused.
fn replay(
    tx: &Transaction<'_>,
    actor: &str,
    path: &str,
    key: &str,
    digest: &str,
) -> Result<Option<Stored>> {
    let Some(found) = stored(tx, actor, path, key)? else {
        return Ok(None);
    };
    if found.digest != digest {
        return Err(ApiFailure::new(
            ErrorCode::IdempotencyConflict,
            "This Idempotency-Key was used for a different request.",
        ));
    }
    Ok(Some(found))
}

fn status_of(found: &Stored) -> StatusCode {
    StatusCode::from_u16(found.status).unwrap_or(StatusCode::OK)
}

/// An answer's bytes. One that cannot be encoded fails the write, so no
/// empty answer is ever stored under a key.
fn encode(body: &serde_json::Value) -> Result<Vec<u8>> {
    serde_json::to_vec(body).map_err(|_| ApiFailure::unavailable())
}

#[allow(clippy::too_many_arguments)]
fn record(
    tx: &Transaction<'_>,
    actor: &str,
    path: &str,
    key: &str,
    digest: &str,
    status: StatusCode,
    body: &[u8],
    now: u64,
) -> Result<()> {
    tx.execute(
        "INSERT INTO idempotency_records (actor, method, path, idempotency_key, command_digest, status, response, created_at)
         VALUES (?1, 'POST', ?2, ?3, ?4, ?5, ?6, ?7)",
        params![actor, path, key, digest, status.as_u16(), body, now],
    )?;
    Ok(())
}

/// A provider's or organizer's idempotent write to `path`. The route
/// authenticates, reads the key and parses and checks its command; `run`
/// does the rest.
pub struct ServiceCall {
    path: String,
}

impl ServiceCall {
    pub fn new(path: impl Into<String>) -> Self {
        Self { path: path.into() }
    }

    /// The command's digest, then `work` in one write transaction under the
    /// actor's key (`idempotent_bytes`), given the command and its digest.
    /// Answers with what `work` answered or, for a repeat, what the first
    /// request was answered.
    pub async fn run<C>(
        self,
        state: &AppState,
        actor: String,
        key: String,
        command: C,
        work: impl FnOnce(&Transaction<'_>, &Ctx, &str, C) -> Result<(StatusCode, serde_json::Value)>
        + Send
        + 'static,
    ) -> Result<Response>
    where
        C: Serialize + Send + 'static,
    {
        let digest = json::digest(&command)?;
        let ctx = Ctx::of(state);
        let Self { path } = self;
        let (status, body) = state
            .db
            .write(move |tx| {
                idempotent_bytes(tx, &actor, &path, &key, &digest, ctx.now, |tx| {
                    work(tx, &ctx, &digest, command)
                })
            })
            .await?;
        Ok(crate::http::json_bytes(status, body))
    }
}

/// The answer stored under a key: the digest of the request that made it,
/// its status and its body.
pub struct Stored {
    pub digest: String,
    pub status: u16,
    pub body: Vec<u8>,
}

/// The answer stored under `key` for `actor` on `path`, if any.
pub fn stored(
    tx: &Transaction<'_>,
    actor: &str,
    path: &str,
    key: &str,
) -> rusqlite::Result<Option<Stored>> {
    tx.query_row(
        "SELECT command_digest, status, response FROM idempotency_records
         WHERE actor = ?1 AND method = 'POST' AND path = ?2 AND idempotency_key = ?3",
        params![actor, path, key],
        |row| {
            Ok(Stored {
                digest: row.get(0)?,
                status: row.get(1)?,
                body: row.get(2)?,
            })
        },
    )
    .optional()
}

/// What a refused key stores in place of a request's digest. No digest is
/// this, so every later request under the key meets `IdempotencyConflict`.
pub const REFUSED: &str = "refused";

/// Refuses every later request under `key` for `actor` on `path`, replacing
/// whatever answer it stored. `mark` stands in for the digest: `REFUSED`, or
/// a route's own mark, never a digest (43 base64url characters), when the
/// route must tell its refusals apart on a repeat.
pub fn refuse(
    tx: &Transaction<'_>,
    actor: &str,
    path: &str,
    key: &str,
    mark: &str,
    now: u64,
) -> rusqlite::Result<()> {
    tx.execute(
        "INSERT INTO idempotency_records (actor, method, path, idempotency_key, command_digest, status, response, created_at)
         VALUES (?1, 'POST', ?2, ?3, ?4, 409, ?5, ?6)
         ON CONFLICT (actor, method, path, idempotency_key) DO UPDATE SET
           command_digest = excluded.command_digest, status = excluded.status,
           response = excluded.response, created_at = excluded.created_at",
        params![
            actor,
            path,
            key,
            mark,
            serde_json::json!({ "code": "idempotency_conflict" }).to_string().into_bytes(),
            now
        ],
    )?;
    Ok(())
}
