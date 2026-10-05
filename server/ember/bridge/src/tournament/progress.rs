//! Moving the bracket on: starting sets whose players are free, settling
//! walkovers, and finishing the tournament.
use std::collections::{BTreeMap, BTreeSet};

use ember_protocol::{
    EmberId,
    event::Kind,
    json,
    matches::{MAX_METADATA_ENTRIES, Participant as Assigned, Rules},
    tournament::{self as bracket, Format, Source, Status},
};
use rusqlite::{Transaction, params};
use serde_json::json;

use super::{
    MATCH_PREFIX, Tournament, load,
    standings::round_robin_table,
    state::{Bracket, Entrant, load_bracket, save, set_json},
    touch, tournament_event,
};
use crate::{
    ctx::Ctx,
    error::{ApiFailure, Result},
    routes::{
        lobbies,
        matches::{self, NewMatch, ORGANIZER_PROFILE},
    },
};

/// Creates the match of a ready node when both players are linked and free.
fn start_node(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament: &Tournament,
    bracket: &Bracket,
    node: usize,
) -> Result<Option<String>> {
    let state = &bracket.states[node];
    let (Some(first), Some(second)) = (
        bracket.entrant(state.slots[0].entrant()),
        bracket.entrant(state.slots[1].entrant()),
    ) else {
        return Ok(None);
    };
    for entrant in [first, second] {
        if !matches::linked(
            tx,
            &tournament.connection_id,
            &entrant.participant_id,
            &entrant.ember_id,
        )? || matches::busy(tx, &tournament.connection_id, &entrant.ember_id, "")?
        {
            return Ok(None);
        }
    }
    let plan = &bracket.plan[node];
    let games_to_win = if plan.finals {
        tournament.finals_games_to_win
    } else {
        tournament.games_to_win
    };
    let number = bracket.counts[node] + 1;
    let external_match_id = format!("{MATCH_PREFIX}{}:{node}.{number}", tournament.id);
    let digest =
        json::digest(&json!({ "tournament_id": tournament.id, "node": node, "match": number }))
            .map_err(|_| ApiFailure::unavailable())?;
    let mut metadata = BTreeMap::from([
        ("round_label".to_owned(), plan.label.clone()),
        ("tournament_node".to_owned(), node.to_string()),
    ]);
    for (key, value) in &tournament.metadata {
        if metadata.len() >= MAX_METADATA_ENTRIES {
            break;
        }
        metadata.entry(key.clone()).or_insert_with(|| value.clone());
    }
    let rules = Rules::standard(games_to_win, ORGANIZER_PROFILE);
    let roster = [first, second].map(|entrant| entrant.clone());
    let assigned = [0u8, 1].map(|slot| Assigned {
        participant_id: roster[usize::from(slot)].participant_id.clone(),
        ember_id: roster[usize::from(slot)].ember_id.clone(),
        slot,
    });
    let match_id = matches::insert_match(
        tx,
        ctx,
        &NewMatch {
            tenant_id: &tournament.tenant_id,
            connection_id: &tournament.connection_id,
            external_match_id: &external_match_id,
            digest: &digest,
            rules: &rules,
            required_build_id: &tournament.required_build_id,
            metadata: &metadata,
            participants: [&assigned[0], &assigned[1]],
            lobby_id: None,
            tournament_id: Some(&tournament.id),
        },
    )?;
    Ok(Some(match_id))
}

/// Settles what needs no match, starts every set whose players are free, and
/// finishes the tournament when nothing is left. A playing set that a
/// withdrawal decided has its match cancelled; those players are returned.
pub(super) fn progress(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament: &Tournament,
    bracket: &mut Bracket,
) -> Result<Vec<EmberId>> {
    if tournament.state != "running" {
        return Ok(Vec::new());
    }
    let before = bracket.states.clone();
    let withdrawn = bracket.withdrawn();
    let changed = bracket::settle(&bracket.plan, &mut bracket.states, &withdrawn);
    let mut freed = Vec::new();
    for &node in &changed {
        if before[node].status == Status::Playing
            && let Some(match_id) = bracket.matches[node].clone()
        {
            freed.extend(matches::cancel_tournament_set(
                tx,
                ctx,
                &match_id,
                "entrant_withdrew",
            )?);
        }
    }
    let mut started = Vec::new();
    for node in 0..bracket.plan.len() {
        if bracket.states[node].status == Status::Ready
            && let Some(match_id) = start_node(tx, ctx, tournament, bracket, node)?
        {
            bracket.states[node].status = Status::Playing;
            bracket.matches[node] = Some(match_id);
            bracket.counts[node] += 1;
            started.push(node);
        }
    }
    if changed.is_empty() && started.is_empty() {
        if bracket::finished(&bracket.states) {
            finish(tx, ctx, tournament, bracket)?;
            wake_lobbies(tx, ctx, tournament, bracket)?;
        }
        return Ok(freed);
    }
    let touched: BTreeSet<usize> = changed.iter().chain(&started).copied().collect();
    save(tx, &tournament.id, bracket, touched, ctx.now)?;
    let revision = touch(tx, &tournament.id, ctx.now)?;
    for &node in &changed {
        if bracket.states[node].status == Status::Walkover {
            let state = bracket.states[node];
            let mut data = set_json(bracket, node);
            data["walkover"] = true.into();
            data["winner"] = bracket.entrant(state.winner()).map(Entrant::public).into();
            data["loser"] = bracket.entrant(state.loser()).map(Entrant::public).into();
            data["eliminated"] = json!(
                bracket
                    .entrant(bracket::eliminated(&bracket.plan, &bracket.states, node))
                    .map(|entrant| &entrant.ember_id)
            );
            data["winner_next"] = bracket.next(node, Source::Winner).into();
            tournament_event(
                tx,
                ctx,
                tournament,
                Kind::TournamentMatchCompleted,
                data,
                revision,
            )?;
        }
    }
    for &node in &started {
        let state = bracket.states[node];
        let mut data = set_json(bracket, node);
        data["match_id"] = bracket.matches[node].clone().into();
        data["games_to_win"] = if bracket.plan[node].finals {
            tournament.finals_games_to_win
        } else {
            tournament.games_to_win
        }
        .into();
        data["participants"] = json!(state.slots.map(|slot| bracket.slot(slot, true)));
        tournament_event(
            tx,
            ctx,
            tournament,
            Kind::TournamentMatchStarted,
            data,
            revision,
        )?;
    }
    if bracket::finished(&bracket.states) {
        finish(tx, ctx, tournament, bracket)?;
    }
    wake_lobbies(tx, ctx, tournament, bracket)?;
    Ok(freed)
}

/// Lobbies skip a player whose bracket set is ready; once the bracket has
/// moved, they look at its entrants again.
fn wake_lobbies(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament: &Tournament,
    bracket: &Bracket,
) -> Result<()> {
    let players: Vec<EmberId> = bracket
        .seeds
        .iter()
        .map(|entrant| entrant.ember_id.clone())
        .collect();
    lobbies::on_players_free(tx, ctx, &tournament.connection_id, &players)
}

/// Places every entrant and closes the tournament.
fn finish(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament: &Tournament,
    bracket: &Bracket,
) -> Result<()> {
    let places = if tournament.format == Format::RoundRobin {
        let table = round_robin_table(tx, bracket)?;
        let mut places = vec![0u32; bracket.seeds.len()];
        for (rank, row) in table.iter().enumerate() {
            places[row.seed] = rank as u32 + 1;
        }
        places
    } else {
        bracket::placements(&bracket.plan, &bracket.states, bracket.seeds.len())
            .ok_or_else(ApiFailure::unavailable)?
    };
    for (seed, place) in places.iter().enumerate() {
        tx.execute(
            "UPDATE tournament_entrants SET placement = ?3, updated_at = ?4 WHERE tournament_id = ?1 AND seed = ?2",
            params![tournament.id, seed, place, ctx.now],
        )?;
    }
    tx.execute(
        "UPDATE tournaments SET state = 'completed' WHERE id = ?1",
        [&tournament.id],
    )?;
    let revision = touch(tx, &tournament.id, ctx.now)?;
    let mut order: Vec<usize> = (0..places.len()).collect();
    order.sort_by_key(|&seed| (places[seed], seed));
    let placements: Vec<_> = order
        .into_iter()
        .map(|seed| {
            let mut player = bracket.seeds[seed].public();
            player["placement"] = places[seed].into();
            player
        })
        .collect();
    tournament_event(
        tx,
        ctx,
        tournament,
        Kind::TournamentCompleted,
        json!({ "format": tournament.format, "placements": placements }),
        revision,
    )?;
    Ok(())
}

/// A match on the connection ended: tournaments those players are in may
/// start the sets that waited for them.
pub fn on_players_free(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    players: &[EmberId],
) -> Result<()> {
    let mut ids = BTreeSet::new();
    for ember_id in players {
        let found = tx
            .prepare(
                "SELECT t.id FROM tournaments t JOIN tournament_entrants e ON e.tournament_id = t.id
                 WHERE t.connection_id = ?1 AND t.state = 'running' AND e.ember_id = ?2 AND e.state = 'registered'",
            )?
            .query_map(params![connection_id, ember_id.as_str()], |row| row.get::<_, String>(0))?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        ids.extend(found);
    }
    for id in ids {
        let tournament = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
        let mut bracket = load_bracket(tx, &tournament)?;
        progress(tx, ctx, &tournament, &mut bracket)?;
    }
    Ok(())
}

/// Whether a player has a set ready to start in a running tournament on the
/// connection, so a lobby should not seat them.
pub fn waiting(tx: &Transaction<'_>, connection_id: &str, ember_id: &EmberId) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM tournament_nodes n JOIN tournaments t ON t.id = n.tournament_id
           JOIN tournament_entrants e ON e.tournament_id = t.id
          WHERE t.connection_id = ?1 AND t.state = 'running' AND e.ember_id = ?2 AND e.state = 'registered'
            AND n.status = 'ready' AND (n.slot_a = e.seed OR n.slot_b = e.seed))",
        params![connection_id, ember_id.as_str()],
        |row| row.get(0),
    )?)
}
