//! `POST /v1/blumint/matches/close`: BluMint no longer needs a match the
//! bridge made for it, because the bracket match was decided, replaced or
//! removed without it, or its host took it over. BluMint waits for the
//! close before it creates the players' next match, so a closed match frees
//! its players and is sent nothing more.
//!
//! A close names the match by its creation's `Idempotency-Key` (`keys`):
//! whatever the bridge made under that key is closed, and the key refuses any
//! creation from then on, since one still on its way would make a match
//! nobody plays or closes.
//!
//! A bridge older than BluMint's 2.0.0 API named its BluMint matches by an
//! ID of its own, so BluMint cannot close a match it left open: the bridge
//! ends each such match as it starts (`end_unkeyed`).
use axum::{extract::State, http::HeaderMap, response::Response};
use rusqlite::Transaction;
use serde::Deserialize;
use serde_json::{Value, json};

use super::{BluMintBody, BluMintRefusal, MATCHES_PATH, caller, keys, parse};
use crate::{
    AppState,
    audit::audit,
    ctx::Ctx,
    error::Result,
    http::{GENERAL_BODY, is_idempotency_key, ok},
    routes::{
        idempotency,
        ledger::{Match, load},
        matches::cancel_and_release,
    },
};

/// Most characters of a reason the bridge keeps. BluMint's are short words.
const MAX_REASON: usize = 64;

#[derive(Deserialize)]
struct CloseRequest {
    #[serde(rename = "idempotencyKey")]
    idempotency_key: String,
    /// For the log only, so any value is taken: a reason from a later BluMint,
    /// or none, still closes the match.
    #[serde(default)]
    reason: Value,
}

/// The reason as the log keeps it: its text without control characters, cut
/// to `MAX_REASON` characters, or "unspecified".
fn logged(reason: &Value) -> String {
    let text: String = reason
        .as_str()
        .unwrap_or_default()
        .chars()
        .filter(|c| !c.is_control())
        .take(MAX_REASON)
        .collect();
    if text.trim().is_empty() {
        "unspecified".to_owned()
    } else {
        text
    }
}

/// `POST /v1/blumint/matches/close`. Every reason closes the match, including
/// one the bridge does not know, and the answer is `200` for every key
/// (BluMint's table has no other row that does not make it send the match to
/// its host). A key the bridge cannot have made a match under, one longer
/// than it takes, is a key it holds nothing under, so nothing is recorded.
/// Only a body that is not a close at all, which BluMint never sends, is
/// refused. The close's own `Idempotency-Key` is not read: closing is the
/// same however often it is asked, and BluMint's close keys can be longer
/// than any creation's.
pub async fn close(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: BluMintBody<GENERAL_BODY>,
) -> std::result::Result<Response, BluMintRefusal> {
    let (connection_id, credential) = caller(&state, &headers).await?;
    let request: CloseRequest = body
        .and_then(|body| parse(&body))
        .map_err(BluMintRefusal::classify)?;
    if !is_idempotency_key(&request.idempotency_key) {
        return Ok(ok(&json!({ "closed": true })));
    }
    let reason = format!("closed by BluMint: {}", logged(&request.reason));
    let ctx = Ctx::of(&state);
    state
        .db
        .write_with(move |tx| -> std::result::Result<(), BluMintRefusal> {
            let found = close_key(tx, &ctx, &connection_id, &request.idempotency_key)?;
            if let Some(found) = found {
                end(tx, &ctx, ("service", credential), &found, &reason)?;
            }
            Ok(())
        })
        .await?;
    state.committed();
    Ok(ok(&json!({ "closed": true })))
}

/// Marks a creation key closed, and returns the match it made, if any. A key
/// the bridge holds nothing under is closed the same way, and answers as a
/// match already over does: BluMint reads a close it cannot match to
/// anything as done, and a creation still on its way would otherwise make a
/// match nobody closes.
fn close_key(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    key: &str,
) -> std::result::Result<Option<Match>, BluMintRefusal> {
    let made = keys::made_under(tx, &ctx.tenant_of(connection_id)?, connection_id, key)?;
    idempotency::refuse(
        tx,
        &keys::owner(connection_id),
        MATCHES_PATH,
        key,
        idempotency::REFUSED,
        ctx.now,
    )?;
    let Some(id) = made else {
        return Ok(None);
    };
    Ok(load(tx, &id)?)
}

/// Ends a match BluMint closed, as `actor`, unless it already ended, and
/// sends BluMint nothing more for it.
fn end(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    actor: (&str, String),
    found: &Match,
    reason: &str,
) -> Result<()> {
    if found.state.is_terminal() {
        return Ok(());
    }
    cancel_and_release(tx, ctx, found, reason)?;
    tx.execute(
        "UPDATE matches SET delivery_state = 'not_required' WHERE id = ?1",
        [&found.id],
    )?;
    audit(
        tx,
        ctx.now,
        actor,
        "blumint.close",
        &found.id,
        "ok",
        Some(reason),
    )?;
    Ok(())
}

/// Why a match an older bridge made was ended (`end_unkeyed`).
const UNKEYED_REASON: &str = "closed on upgrade: made before BluMint closed matches by key";

/// Ends, as a close would, every open match on a BluMint connection whose
/// `external_match_id` is not a key (`keys::external_id`). Only a bridge
/// older than BluMint's 2.0.0 API made those, so BluMint cannot close them,
/// and their players would stay busy until the 24-hour cap, which then sends
/// an abort for a match BluMint has already closed. Run as the bridge starts;
/// once those matches have ended it finds nothing.
pub fn end_unkeyed(tx: &Transaction<'_>, ctx: &Ctx) -> Result<()> {
    let ids = tx
        .prepare(
            "SELECT m.id FROM matches m JOIN provider_connections c ON c.id = m.connection_id
             WHERE c.kind = 'blumint' AND m.state NOT IN ('completed', 'cancelled', 'failed', 'expired')
               AND m.external_match_id NOT LIKE 'blumint:%'",
        )?
        .query_map([], |row| row.get::<_, String>(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for id in ids {
        if let Some(found) = load(tx, &id)? {
            end(
                tx,
                ctx,
                ("bridge", "upgrade".into()),
                &found,
                UNKEYED_REASON,
            )?;
        }
    }
    Ok(())
}
