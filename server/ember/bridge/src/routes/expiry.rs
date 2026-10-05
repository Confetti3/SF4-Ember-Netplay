//! Matches nobody plays expire (docs/guides/INTEGRATIONS.md, "Matches nobody
//! plays"). A match stores its current deadline (`expires_at`) and how long a
//! quiet match lives (`expiry_secs`). Creating the match, issuing a permit and
//! settling a game (`extend`, called inside those transactions) each set the
//! deadline to now plus that lifetime, so the snapshot, the assignment list
//! and the sweep all read `expires_at` as it is.
use ember_protocol::{
    event::Kind,
    matches::{DeliveryState, MatchState},
};
use rusqlite::{Transaction, params};
use serde_json::json;

use crate::{
    audit::audit,
    ctx::Ctx,
    error::{ApiFailure, Result},
    routes::{
        ledger::{Match, bump, load, match_event, participants, release},
        policy,
    },
};

/// Matches expired in one pass; the rest wait for the next.
const EXPIRE_BATCH: i64 = 50;
/// An expired match stays in its players' assignments this long, so the game
/// can show it as expired instead of it vanishing.
pub const EXPIRED_LISTED_SECS: u64 = 24 * 60 * 60;

/// How long a new match lives while quiet. A match nobody plays expires. A
/// lobby or bracket set ends with its lobby or tournament instead (a bracket
/// waits on its sets), so it has no expiry.
pub fn lifetime(ctx: &Ctx, is_set: bool) -> Option<u64> {
    (!is_set).then(|| ctx.config.match_expiry_secs())
}

/// Game activity: moves the match's deadline to a whole lifetime from now, so
/// a set the players are working through is never cut off. Called inside the
/// transaction that issues a permit or settles a game (a result, an abort, a
/// voided game or a correction). Never moves a deadline earlier.
pub fn extend(tx: &Transaction<'_>, now: u64, match_id: &str) -> Result<()> {
    tx.execute(
        "UPDATE matches SET expires_at = MAX(expires_at, ?1 + expiry_secs)
         WHERE id = ?2 AND expiry_secs IS NOT NULL",
        params![now, match_id],
    )?;
    Ok(())
}

/// Expires the matches nobody played. A match expires once its deadline has
/// passed and nothing is happening in it: no game open (permitted, or held
/// for review) and no live provisioning lease. One waiting for an organizer's
/// review (`needs_review`) never does. It ends as `expired`, not `cancelled`,
/// so a platform does not read it as a request to play it again, and its
/// players are free as after a cancellation.
pub fn expire_stale(tx: &Transaction<'_>, ctx: &Ctx) -> Result<()> {
    let ids = tx
        .prepare(
            "SELECT m.id FROM matches m
             WHERE m.expires_at IS NOT NULL AND m.expires_at <= ?1
               AND m.state NOT IN ('completed', 'cancelled', 'failed', 'expired', 'needs_review')
               AND NOT EXISTS (SELECT 1 FROM attempts a WHERE a.match_id = m.id AND a.state IN ('permitted', 'review'))
               AND NOT EXISTS (SELECT 1 FROM match_rooms r WHERE r.match_id = m.id AND r.lease_expires_at > ?1)
             ORDER BY m.expires_at, m.id LIMIT ?2",
        )?
        .query_map(params![ctx.now, EXPIRE_BATCH], |row| row.get::<_, String>(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for id in ids {
        let found = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
        expire_and_release(tx, ctx, &found)?;
    }
    Ok(())
}

fn expire_and_release(tx: &Transaction<'_>, ctx: &Ctx, found: &Match) -> Result<()> {
    let revision = bump(tx, found, MatchState::Expired, ctx.now)?;
    let expires_at: u64 = tx.query_row(
        "SELECT expires_at FROM matches WHERE id = ?1",
        [&found.id],
        |row| row.get(0),
    )?;
    match_event(
        tx,
        ctx,
        found,
        Kind::MatchExpired,
        json!({
            "match_id": found.id,
            "match_revision": revision.to_string(),
            "state": MatchState::Expired,
            "reason": "not_played",
            "expires_at": expires_at,
        }),
    )?;
    // A platform that cannot be told is sent nothing, and its result is no
    // longer queued (an unsent one would be leased again and again).
    if policy::of(tx, &found.connection_id)?.silent_expiry
        && found.delivery == DeliveryState::Queued
    {
        tx.execute(
            "UPDATE matches SET delivery_state = 'not_required' WHERE id = ?1 AND delivery_state = 'queued'",
            [&found.id],
        )?;
    }
    audit(
        tx,
        ctx.now,
        ("bridge", "maintenance".into()),
        "match.expire",
        &found.id,
        "ok",
        Some("not_played"),
    )?;
    release(
        tx,
        ctx,
        found,
        &participants(tx, &found.id, found.generation)?,
    )
}
