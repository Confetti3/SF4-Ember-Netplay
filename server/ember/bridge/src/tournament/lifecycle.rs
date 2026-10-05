//! A tournament's life: opening registration, entrants coming and going,
//! the start and the cancellation. Each function is the whole change, events
//! and audit row included, for the caller's transaction. Who may do it and
//! the revision it was based on are checked by the caller first.
use std::collections::BTreeSet;

use ember_protocol::{
    EmberId,
    api::ErrorCode,
    event::Kind,
    tournament::{self as bracket, CreateTournament, Format, RegisterEntrant, Status},
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde_json::json;

use super::{
    Applied, Tournament, closed, load,
    progress::progress,
    state::{Entrant, entrants, load_bracket, registered_count},
    touch, tournament_event,
};
use crate::{
    audit::audit,
    ctx::Ctx,
    error::{ApiFailure, Result},
    routes::{lobbies, matches},
    util::new_id,
};

/// Opens registration on a connection. The same `external_tournament_id`
/// with the same command answers with the tournament it named.
pub fn create(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tenant_id: &str,
    connection_id: &str,
    command: &CreateTournament,
    digest: &str,
) -> Result<Applied> {
    let existing: Option<(String, String)> = tx
        .query_row(
            "SELECT id, create_digest FROM tournaments
             WHERE tenant_id = ?1 AND connection_id = ?2 AND external_tournament_id = ?3",
            params![tenant_id, connection_id, command.external_tournament_id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?;
    if let Some((id, stored)) = existing {
        if stored != digest {
            return Err(ApiFailure::new(
                ErrorCode::IdempotencyConflict,
                "That external_tournament_id already names a different tournament.",
            ));
        }
        let tournament = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
        return Ok(Applied::Unchanged(tournament));
    }
    let id = new_id("etn");
    let finals = command.finals_games_to_win.unwrap_or(command.games_to_win);
    let reset = command.grand_final_reset && command.format == Format::DoubleElimination;
    tx.execute(
        "INSERT INTO tournaments (id, tenant_id, connection_id, external_tournament_id, create_digest, revision,
            state, format, games_to_win, finals_games_to_win, grand_final_reset, required_build_id, metadata,
            created_at, updated_at)
         VALUES (?1, ?2, ?3, ?4, ?5, 1, 'registration', ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?12)",
        params![
            id,
            tenant_id,
            connection_id,
            command.external_tournament_id,
            digest,
            command.format.as_str(),
            command.games_to_win,
            finals,
            reset,
            command.required_build_id,
            serde_json::to_string(&command.metadata).unwrap_or_default(),
            ctx.now
        ],
    )?;
    let tournament = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
    tournament_event(
        tx,
        ctx,
        &tournament,
        Kind::TournamentCreated,
        json!({
            "external_tournament_id": command.external_tournament_id,
            "format": command.format,
            "games_to_win": command.games_to_win,
            "finals_games_to_win": finals,
            "grand_final_reset": reset,
            "metadata": command.metadata,
        }),
        1,
    )?;
    Ok(Applied::Changed(tournament))
}

/// Registers a linked player before the tournament starts. Registering again
/// changes nothing.
pub fn register(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament: Tournament,
    command: &RegisterEntrant,
) -> Result<Applied> {
    if tournament.state != "registration" {
        return Err(ApiFailure::new(
            ErrorCode::StaleRevision,
            "Registration for this tournament is closed.",
        ));
    }
    if !matches::linked(
        tx,
        &tournament.connection_id,
        &command.participant_id,
        &command.ember_id,
    )? {
        return Err(ApiFailure::invalid(
            "That participant is not currently linked to that Ember ID on this connection.",
        ));
    }
    let current: Option<String> = tx
        .query_row(
            "SELECT state FROM tournament_entrants WHERE tournament_id = ?1 AND ember_id = ?2",
            params![tournament.id, command.ember_id.as_str()],
            |row| row.get(0),
        )
        .optional()?;
    if current.as_deref() == Some("registered") {
        return Ok(Applied::Unchanged(tournament));
    }
    let count = registered_count(tx, &tournament.id)?;
    let max = tournament.format.max_entrants() as i64;
    if count >= max {
        return Err(
            ApiFailure::new(ErrorCode::RateLimited, "The tournament is full.")
                .detail("max_entrants", max),
        );
    }
    tx.execute(
        "INSERT INTO tournament_entrants (tournament_id, ember_id, participant_id, state, seed, placement,
            registered_at, updated_at)
         VALUES (?1, ?2, ?3, 'registered', NULL, NULL, ?4, ?4)
         ON CONFLICT (tournament_id, ember_id) DO UPDATE SET state = 'registered',
            participant_id = excluded.participant_id, registered_at = excluded.registered_at,
            updated_at = excluded.updated_at",
        params![tournament.id, command.ember_id.as_str(), command.participant_id, ctx.now],
    )?;
    let revision = touch(tx, &tournament.id, ctx.now)?;
    tournament_event(
        tx,
        ctx,
        &tournament,
        Kind::TournamentEntrantsChanged,
        json!({ "reason": "registered", "ember_id": command.ember_id, "entrants": count + 1 }),
        revision,
    )?;
    let tournament = load(tx, &tournament.id)?.ok_or_else(ApiFailure::unavailable)?;
    Ok(Applied::Changed(tournament))
}

/// Withdraws the registered player with this `participant_id`, at their
/// request or for a no-show. A participant who is not registered changes
/// nothing. Returns the tournament as it now stands.
pub fn withdraw_participant(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament: &Tournament,
    participant: &str,
    actor: (&str, String),
) -> Result<Tournament> {
    if !matches!(tournament.state.as_str(), "registration" | "running") {
        return Err(closed());
    }
    let ember_id: Option<String> = tx
        .query_row(
            "SELECT ember_id FROM tournament_entrants WHERE tournament_id = ?1 AND participant_id = ?2
               AND state = 'registered'",
            params![tournament.id, participant],
            |row| row.get(0),
        )
        .optional()?;
    if let Some(ember_id) = ember_id {
        let ember_id = EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?;
        withdraw(tx, ctx, tournament, &ember_id, "withdrew")?;
        audit(
            tx,
            ctx.now,
            actor,
            "tournament.withdraw",
            &tournament.id,
            "ok",
            None,
        )?;
    }
    load(tx, &tournament.id)?.ok_or_else(ApiFailure::unavailable)
}

/// Closes registration, seeds the entrants and starts the first sets.
/// `seeding` lists every registered `participant_id`, top seed first;
/// registration order when absent.
pub fn start(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament: &Tournament,
    seeding: Option<&[String]>,
    actor: (&str, String),
) -> Result<Tournament> {
    if tournament.state != "registration" {
        return Err(ApiFailure::new(
            ErrorCode::StaleRevision,
            "The tournament has already started.",
        ));
    }
    let registered: Vec<Entrant> = entrants(tx, &tournament.id)?
        .into_iter()
        .filter(|entrant| !entrant.withdrawn)
        .collect();
    if registered.len() < 2 {
        return Err(ApiFailure::invalid(
            "A tournament needs at least two entrants.",
        ));
    }
    let order: Vec<&Entrant> = match seeding {
        None => registered.iter().collect(),
        Some(seeding) => {
            let mut seen = BTreeSet::new();
            let order: Vec<&Entrant> = seeding
                .iter()
                .filter(|participant| seen.insert(participant.as_str()))
                .filter_map(|participant| {
                    registered
                        .iter()
                        .find(|entrant| entrant.participant_id == *participant)
                })
                .collect();
            if order.len() != registered.len() || seeding.len() != registered.len() {
                return Err(ApiFailure::invalid(
                    "seeding must list every registered participant_id exactly once.",
                ));
            }
            order
        }
    };
    let plan = bracket::plan(tournament.format, order.len(), tournament.grand_final_reset)
        .map_err(|_| ApiFailure::invalid("That entrant count is not supported for this format."))?;
    for (seed, entrant) in order.iter().enumerate() {
        tx.execute(
            "UPDATE tournament_entrants SET seed = ?3, updated_at = ?4 WHERE tournament_id = ?1 AND ember_id = ?2",
            params![tournament.id, entrant.ember_id.as_str(), seed, ctx.now],
        )?;
    }
    for node in 0..plan.len() {
        tx.execute(
            "INSERT INTO tournament_nodes (tournament_id, node, slot_a, slot_b, status, winner_slot, match_id,
                match_count, updated_at)
             VALUES (?1, ?2, NULL, NULL, 'pending', NULL, NULL, 0, ?3)",
            params![tournament.id, node, ctx.now],
        )?;
    }
    tx.execute(
        "UPDATE tournaments SET state = 'running' WHERE id = ?1",
        [&tournament.id],
    )?;
    let revision = touch(tx, &tournament.id, ctx.now)?;
    let seeds: Vec<_> = order
        .iter()
        .enumerate()
        .map(|(seed, entrant)| {
            let mut player = entrant.public();
            player["seed"] = (seed + 1).into();
            player
        })
        .collect();
    let tournament = load(tx, &tournament.id)?.ok_or_else(ApiFailure::unavailable)?;
    tournament_event(
        tx,
        ctx,
        &tournament,
        Kind::TournamentStarted,
        json!({ "format": tournament.format, "entrants": seeds, "sets": plan.len() }),
        revision,
    )?;
    audit(
        tx,
        ctx.now,
        actor,
        "tournament.start",
        &tournament.id,
        "ok",
        None,
    )?;
    let mut bracket = load_bracket(tx, &tournament)?;
    progress(tx, ctx, &tournament, &mut bracket)?;
    load(tx, &tournament.id)?.ok_or_else(ApiFailure::unavailable)
}

/// Cancels the tournament. Sets being played have their matches cancelled,
/// and entrants a ready set held back from lobbies are free again.
pub fn cancel(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament: &Tournament,
    reason: &str,
    actor: (&str, String),
) -> Result<Tournament> {
    if !matches!(tournament.state.as_str(), "registration" | "running") {
        return Err(closed());
    }
    let mut freed = Vec::new();
    if tournament.state == "running" {
        let bracket = load_bracket(tx, tournament)?;
        for (node, state) in bracket.states.iter().enumerate() {
            if state.status == Status::Playing
                && let Some(match_id) = &bracket.matches[node]
            {
                freed.extend(matches::cancel_tournament_set(
                    tx,
                    ctx,
                    match_id,
                    "tournament_cancelled",
                )?);
            }
        }
    }
    tx.execute(
        "UPDATE tournaments SET state = 'cancelled' WHERE id = ?1",
        [&tournament.id],
    )?;
    let revision = touch(tx, &tournament.id, ctx.now)?;
    tournament_event(
        tx,
        ctx,
        tournament,
        Kind::TournamentCancelled,
        json!({ "reason": reason }),
        revision,
    )?;
    audit(
        tx,
        ctx.now,
        actor,
        "tournament.cancel",
        &tournament.id,
        "ok",
        Some(reason),
    )?;
    matches::release_players(tx, ctx, &tournament.connection_id, &freed)?;
    // Entrants a ready set held back from lobbies are free now.
    let everyone: Vec<EmberId> = entrants(tx, &tournament.id)?
        .into_iter()
        .map(|entrant| entrant.ember_id)
        .collect();
    lobbies::on_players_free(tx, ctx, &tournament.connection_id, &everyone)?;
    load(tx, &tournament.id)?.ok_or_else(ApiFailure::unavailable)
}

/// An ended link withdraws that player from the connection's tournaments.
pub fn on_unlink(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    ember_id: &EmberId,
) -> Result<()> {
    let ids = tx
        .prepare(
            "SELECT t.id FROM tournaments t JOIN tournament_entrants e ON e.tournament_id = t.id
             WHERE t.connection_id = ?1 AND t.state IN ('registration', 'running') AND e.ember_id = ?2
               AND e.state = 'registered'",
        )?
        .query_map(params![connection_id, ember_id.as_str()], |row| row.get::<_, String>(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for id in ids {
        let tournament = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
        withdraw(tx, ctx, &tournament, ember_id, "identity_unlinked")?;
    }
    Ok(())
}

/// Takes a registered player out. Before the start that is all; once running,
/// every set they have left is a walkover for their opponent, and a set they
/// are playing has its match cancelled.
fn withdraw(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament: &Tournament,
    ember_id: &EmberId,
    reason: &str,
) -> Result<()> {
    let changed = tx.execute(
        "UPDATE tournament_entrants SET state = 'withdrawn', updated_at = ?3
         WHERE tournament_id = ?1 AND ember_id = ?2 AND state = 'registered'",
        params![tournament.id, ember_id.as_str(), ctx.now],
    )?;
    if changed == 0 {
        return Ok(());
    }
    let revision = touch(tx, &tournament.id, ctx.now)?;
    tournament_event(
        tx,
        ctx,
        tournament,
        Kind::TournamentEntrantsChanged,
        json!({ "reason": reason, "ember_id": ember_id, "entrants": registered_count(tx, &tournament.id)? }),
        revision,
    )?;
    if tournament.state == "running" {
        let mut bracket = load_bracket(tx, tournament)?;
        let freed = progress(tx, ctx, tournament, &mut bracket)?;
        matches::release_players(tx, ctx, &tournament.connection_id, &freed)?;
    }
    Ok(())
}
