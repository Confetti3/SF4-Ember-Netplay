//! How a match ends early or waits for a person, whoever ends it: a cancel
//! (by its provider or organizer, its lobby or its tournament), an ended link
//! and a result that needs review.
use ember_protocol::{EmberId, event::Kind, matches::MatchState};
use rusqlite::{Transaction, params};
use serde::Serialize;
use serde_json::json;

use super::{Match, bump, load, load_listed, match_event, participants, release};
use crate::{
    ctx::Ctx,
    error::{ApiFailure, Result},
    routes::lobbies,
    tournament,
};

/// Cancels a match and returns its players to their other matches, as the
/// cancel route does. Returns the new revision.
pub fn cancel_and_release(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    found: &Match,
    reason: &str,
) -> Result<u64> {
    let revision = cancel_match(tx, ctx, found, reason)?;
    release(
        tx,
        ctx,
        found,
        &participants(tx, &found.id, found.generation)?,
    )?;
    Ok(revision)
}

fn cancel_match(tx: &Transaction<'_>, ctx: &Ctx, found: &Match, reason: &str) -> Result<u64> {
    // A game the fighters were playing counts for nobody; reports that come
    // later are kept as evidence only.
    tx.execute(
        "UPDATE attempts SET state = 'aborted' WHERE match_id = ?1 AND state IN ('permitted', 'review')",
        [&found.id],
    )?;
    let revision = bump(tx, found, MatchState::Cancelled, ctx.now)?;
    match_event(
        tx,
        ctx,
        found,
        Kind::MatchCancelled,
        json!({ "match_id": found.id, "match_revision": revision.to_string(), "state": "cancelled", "reason": reason }),
    )?;
    Ok(revision)
}

/// Cancels a lobby's running set when a seated player leaves or the lobby
/// closes. A finished set is left as it is. A tournament set waiting on its
/// players can start; the lobby itself moves on in its own code.
pub fn cancel_lobby_set(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    match_id: &str,
    reason: &str,
) -> Result<()> {
    let found = load(tx, match_id)?.ok_or_else(ApiFailure::unavailable)?;
    if !found.state.is_terminal() {
        cancel_match(tx, ctx, &found, reason)?;
        let players: Vec<EmberId> = participants(tx, &found.id, found.generation)?
            .into_iter()
            .map(|p| p.ember_id)
            .collect();
        tournament::on_players_free(tx, ctx, &found.connection_id, &players)?;
    }
    Ok(())
}

/// Cancels a tournament set that a withdrawal, a correction or the
/// tournament's end has made moot, returning its players.
pub fn cancel_tournament_set(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    match_id: &str,
    reason: &str,
) -> Result<Vec<EmberId>> {
    let found = load(tx, match_id)?.ok_or_else(ApiFailure::unavailable)?;
    if found.state.is_terminal() {
        return Ok(Vec::new());
    }
    cancel_match(tx, ctx, &found, reason)?;
    Ok(participants(tx, &found.id, found.generation)?
        .into_iter()
        .map(|p| p.ember_id)
        .collect())
}

/// Whether a match has an accepted game.
pub fn has_games(tx: &Transaction<'_>, match_id: &str) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM attempts WHERE match_id = ?1 AND state = 'accepted')",
        [match_id],
        |row| row.get(0),
    )?)
}

/// An unlinked identity cannot keep playing an assigned match: every active
/// match on that connection that names it goes to review (spec 11.6). Its
/// lobby places end instead, which cancels a lobby set it was playing.
pub fn on_unlink(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    _tenant: &str,
    connection_id: &str,
    ember_id: &EmberId,
) -> Result<()> {
    lobbies::on_unlink(tx, ctx, connection_id, ember_id)?;
    tournament::on_unlink(tx, ctx, connection_id, ember_id)?;
    let ids = tx
        .prepare(
            "SELECT m.id FROM matches m JOIN match_participants p
               ON p.match_id = m.id AND p.assignment_generation = m.assignment_generation
             WHERE m.connection_id = ?1 AND p.ember_id = ?2 AND m.lobby_id IS NULL AND m.tournament_id IS NULL
               AND m.state NOT IN ('completed', 'cancelled', 'failed', 'expired', 'needs_review')",
        )?
        .query_map(params![connection_id, ember_id.as_str()], |row| {
            row.get::<_, String>(0)
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for id in ids {
        let Some(found) = load_listed(tx, &id)? else {
            continue;
        };
        // A game in progress waits for the organizer with the rest of the match.
        tx.execute(
            "UPDATE attempts SET state = 'review' WHERE match_id = ?1 AND state = 'permitted'",
            [&id],
        )?;
        needs_review(tx, ctx, &found, "identity_unlinked", None)?;
    }
    Ok(())
}

/// A match whose result needs a person (spec 16.6) waits in `needs_review`
/// for its organizer. Where the platform has no review
/// (`Policy::reviews_disputes`), it is cancelled instead and its players are
/// free. `attempt_id` is the game held, when one is.
pub fn needs_review(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    found: &Match,
    reason: &str,
    attempt_id: Option<&str>,
) -> Result<()> {
    if !found.policy()?.reviews_disputes {
        cancel_and_release(tx, ctx, found, reason)?;
        return Ok(());
    }
    let revision = bump(tx, found, MatchState::NeedsReview, ctx.now)?;
    let data = serde_json::to_value(NeedsReviewData {
        match_id: &found.id,
        match_revision: revision.to_string(),
        state: MatchState::NeedsReview,
        reason,
        attempt_id,
    })
    .map_err(|_| ApiFailure::unavailable())?;
    match_event(tx, ctx, found, Kind::NeedsReview, data)?;
    Ok(())
}

/// Data of `match.needs_review`.
#[derive(Serialize)]
struct NeedsReviewData<'a> {
    match_id: &'a str,
    match_revision: String,
    state: MatchState,
    reason: &'a str,
    /// The game held, when one is.
    #[serde(skip_serializing_if = "Option::is_none")]
    attempt_id: Option<&'a str>,
}
