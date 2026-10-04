//! Results arriving from the match code: a set completing, and corrections
//! that reopen one.
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    event::Kind,
    matches::Score,
    tournament::{self as bracket, Blocked, Source, Status},
};
use rusqlite::Transaction;
use serde_json::json;

use super::{
    closed, load,
    progress::progress,
    state::{Entrant, load_bracket, save, set_json},
    touch, tournament_event,
};
use crate::{
    ctx::Ctx,
    error::{ApiFailure, Result},
    routes::matches,
};

/// Called by the match code in the transaction that completes a tournament set.
pub fn on_match_completed(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament_id: &str,
    match_id: &str,
    winner_slot: u8,
    scores: &[Score],
) -> Result<()> {
    let tournament = load(tx, tournament_id)?.ok_or_else(ApiFailure::unavailable)?;
    if tournament.state != "running" {
        return Ok(());
    }
    let mut bracket = load_bracket(tx, &tournament)?;
    let Some(node) = bracket
        .matches
        .iter()
        .position(|id| id.as_deref() == Some(match_id))
    else {
        return Ok(());
    };
    if bracket.states[node].status != Status::Playing {
        return Ok(());
    }
    bracket::complete(&mut bracket.states, node, winner_slot)
        .map_err(|_| ApiFailure::unavailable())?;
    save(tx, &tournament.id, &bracket, [node], ctx.now)?;
    let revision = touch(tx, &tournament.id, ctx.now)?;
    let state = bracket.states[node];
    let eliminated = bracket::eliminated(&bracket.plan, &bracket.states, node);
    let mut data = set_json(&bracket, node);
    data["match_id"] = match_id.into();
    data["walkover"] = false.into();
    data["winner"] = bracket.entrant(state.winner()).map(Entrant::public).into();
    data["loser"] = bracket.entrant(state.loser()).map(Entrant::public).into();
    data["scores"] = json!(scores);
    data["eliminated"] = json!(bracket.entrant(eliminated).map(|entrant| &entrant.ember_id));
    data["winner_next"] = bracket.next(node, Source::Winner).into();
    data["loser_next"] = if eliminated.is_some() {
        None
    } else {
        bracket.next(node, Source::Loser)
    }
    .into();
    tournament_event(
        tx,
        ctx,
        &tournament,
        Kind::TournamentMatchCompleted,
        data,
        revision,
    )?;
    let tournament = load(tx, tournament_id)?.ok_or_else(ApiFailure::unavailable)?;
    let freed = progress(tx, ctx, &tournament, &mut bracket)?;
    matches::release_players(tx, ctx, &tournament.connection_id, &freed)
}

/// Called by the match code before any correction of a tournament set that
/// stays finished: a finished or cancelled tournament's results are final.
pub fn check_correction(tx: &Transaction<'_>, tournament_id: &str) -> Result<()> {
    let tournament = load(tx, tournament_id)?.ok_or_else(ApiFailure::unavailable)?;
    match tournament.state.as_str() {
        "running" => Ok(()),
        "completed" => Err(ApiFailure::new(
            ErrorCode::LeaseConflict,
            "The tournament is over, so its results are final.",
        )),
        _ => Err(closed()),
    }
}

/// Called by the match code before a correction reopens a tournament set.
/// Later sets its result fed are cleared, and their matches cancelled while
/// they have no games; a later set that has been played or has a game makes
/// the correction fail. A finished tournament's results are final.
pub fn on_reopen(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament_id: &str,
    match_id: &str,
) -> Result<Vec<EmberId>> {
    check_correction(tx, tournament_id)?;
    let tournament = load(tx, tournament_id)?.ok_or_else(ApiFailure::unavailable)?;
    let mut bracket = load_bracket(tx, &tournament)?;
    let Some(node) = bracket
        .matches
        .iter()
        .position(|id| id.as_deref() == Some(match_id))
    else {
        return Err(ApiFailure::new(
            ErrorCode::LeaseConflict,
            "This set is no longer part of the bracket.",
        ));
    };
    // A set with a player who has since withdrawn would only be a walkover
    // again, and a withdrawn player cannot be put back into play.
    if bracket.states[node]
        .slots
        .iter()
        .filter_map(|slot| slot.entrant())
        .any(|seed| bracket.seeds[seed].withdrawn)
    {
        return Err(ApiFailure::new(
            ErrorCode::LeaseConflict,
            "A player in this set has withdrawn, so its result stands.",
        ));
    }
    let mut games = vec![false; bracket.plan.len()];
    for (later, id) in bracket.matches.iter().enumerate() {
        if let Some(id) = id {
            games[later] = matches::has_games(tx, id)?;
        }
    }
    let cancelled = match bracket::reopen(&bracket.plan, &mut bracket.states, node, |later| {
        games[later]
    }) {
        Ok(cancelled) => cancelled,
        Err(Blocked::Downstream(later)) => {
            return Err(ApiFailure::new(
                ErrorCode::LeaseConflict,
                "A later set this result fed has been played or has a game recorded, so it cannot be reopened.",
            )
            .detail("blocking_set", bracket.plan[later].label.clone()));
        }
        Err(Blocked::NotPlayed) => {
            return Err(ApiFailure::new(
                ErrorCode::LeaseConflict,
                "This set has no result to reopen.",
            ));
        }
    };
    let mut undone = Vec::new();
    let mut freed = Vec::new();
    for &later in &cancelled {
        if let Some(id) = bracket.matches[later].take() {
            // The caller hands these players on once the reopened set's
            // match is active again, so its own players stay reserved for it.
            freed.extend(matches::cancel_tournament_set(
                tx,
                ctx,
                &id,
                "bracket_corrected",
            )?);
            let mut set = set_json(&bracket, later);
            set["match_id"] = id.into();
            undone.push(set);
        }
    }
    // Every set the correction cleared waits for new players; one decided by
    // a walkover keeps no link to its old, already cancelled match.
    for (later, state) in bracket.states.iter().enumerate() {
        if state.status == Status::Pending {
            bracket.matches[later] = None;
        }
    }
    save(tx, &tournament.id, &bracket, 0..bracket.plan.len(), ctx.now)?;
    let revision = touch(tx, &tournament.id, ctx.now)?;
    let mut data = set_json(&bracket, node);
    data["match_id"] = match_id.into();
    data["cancelled"] = json!(undone);
    tournament_event(
        tx,
        ctx,
        &tournament,
        Kind::TournamentMatchReopened,
        data,
        revision,
    )?;
    Ok(freed)
}
