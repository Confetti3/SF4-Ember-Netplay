//! Tournaments: single elimination, double elimination and round robin run
//! by the bridge on a provider connection (an extension beyond EMBER-TB-001).
//!
//! The bracket's layout comes from `ember_protocol::tournament::plan`, worked
//! out again from the format and entrant count whenever it is needed; only
//! each node's state is stored. Each set is an ordinary match, so scoring,
//! adjudication and the busy check are the match code's. When a set completes
//! the bracket moves on in the same transaction: byes and walkovers are
//! decided, and every set whose players are known and free gets its match. A
//! set whose player is busy elsewhere waits, and starts as soon as that match
//! ends; lobbies leave such a player alone so the bracket comes first.
use std::collections::{BTreeMap, BTreeSet};

use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    encoding::{Counter, is_prefixed_id},
    event::Kind,
    json,
    matches::{MAX_METADATA_ENTRIES, Participant as Assigned, Rules, Score},
    tournament::{
        self as bracket, Blocked, CreateTournament, Format, Node, NodeState, RegisterEntrant, Slot,
        Source, Status,
    },
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::{
    AppState,
    auth::{self, Role},
    error::{ApiFailure, Result},
    events::{self, NewEvent, Viewer, emit},
    http::{Body, GENERAL_BODY, expected_revision, idempotency_key, ok},
    routes::{
        links::{Ctx, audit},
        lobbies,
        matches::{
            self, NewMatch, ORGANIZER_PROFILE, idempotent, into_response, service_viewer, viewer_of,
        },
        records,
    },
};

/// `external_match_id` prefix of tournament sets; providers cannot create one.
pub const MATCH_PREFIX: &str = "tournament:";

struct Tournament {
    id: String,
    tenant_id: String,
    connection_id: String,
    external_tournament_id: String,
    revision: u64,
    state: String,
    format: Format,
    games_to_win: u8,
    finals_games_to_win: u8,
    grand_final_reset: bool,
    required_build_id: String,
    metadata: BTreeMap<String, String>,
}

fn load(tx: &Transaction<'_>, id: &str) -> Result<Option<Tournament>> {
    tx.query_row(
        "SELECT id, tenant_id, connection_id, external_tournament_id, revision, state, format, games_to_win,
                finals_games_to_win, grand_final_reset, required_build_id, metadata
         FROM tournaments WHERE id = ?1",
        [id],
        |row| {
            Ok(Tournament {
                id: row.get(0)?,
                tenant_id: row.get(1)?,
                connection_id: row.get(2)?,
                external_tournament_id: row.get(3)?,
                revision: row.get(4)?,
                state: row.get(5)?,
                format: Format::parse(&row.get::<_, String>(6)?).unwrap_or(Format::SingleElimination),
                games_to_win: row.get(7)?,
                finals_games_to_win: row.get(8)?,
                grand_final_reset: row.get(9)?,
                required_build_id: row.get(10)?,
                metadata: serde_json::from_str(&row.get::<_, String>(11)?).unwrap_or_default(),
            })
        },
    )
    .optional()
    .map_err(Into::into)
}

#[derive(Clone, Debug)]
struct Entrant {
    ember_id: EmberId,
    participant_id: String,
    withdrawn: bool,
    seed: Option<usize>,
    placement: Option<u32>,
}

impl Entrant {
    /// For snapshots; a player reading one sees only their own `participant_id`.
    fn player(&self) -> serde_json::Value {
        json!({ "ember_id": self.ember_id, "participant_id": self.participant_id })
    }

    /// For events, which entrants read unchanged: no account handles.
    fn public(&self) -> serde_json::Value {
        json!({ "ember_id": self.ember_id })
    }
}

/// Seeded entrants first, by seed, then the rest in registration order.
fn entrants(tx: &Transaction<'_>, tournament_id: &str) -> Result<Vec<Entrant>> {
    tx.prepare(
        "SELECT ember_id, participant_id, state, seed, placement FROM tournament_entrants
         WHERE tournament_id = ?1 ORDER BY seed IS NULL, seed, registered_at, rowid",
    )?
    .query_map([tournament_id], |row| {
        Ok((
            row.get::<_, String>(0)?,
            row.get::<_, String>(1)?,
            row.get::<_, String>(2)?,
            row.get::<_, Option<usize>>(3)?,
            row.get::<_, Option<u32>>(4)?,
        ))
    })?
    .map(|row| {
        let (ember_id, participant_id, state, seed, placement) = row?;
        Ok(Entrant {
            ember_id: EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?,
            participant_id,
            withdrawn: state == "withdrawn",
            seed,
            placement,
        })
    })
    .collect()
}

/// A running or finished bracket in memory.
struct Bracket {
    plan: Vec<Node>,
    states: Vec<NodeState>,
    matches: Vec<Option<String>>,
    counts: Vec<u32>,
    /// Entrants by seed.
    seeds: Vec<Entrant>,
}

impl Bracket {
    fn withdrawn(&self) -> Vec<bool> {
        self.seeds.iter().map(|entrant| entrant.withdrawn).collect()
    }

    fn slot(&self, slot: Slot, public: bool) -> serde_json::Value {
        match slot {
            Slot::Open => serde_json::Value::Null,
            Slot::Empty => json!({ "bye": true }),
            Slot::Entrant(seed) if public => self.seeds[seed].public(),
            Slot::Entrant(seed) => self.seeds[seed].player(),
        }
    }

    fn entrant(&self, seed: Option<usize>) -> Option<&Entrant> {
        seed.map(|seed| &self.seeds[seed])
    }

    /// The label of the set that a node's winner or loser plays next. A
    /// grand final reset counts only when the losers-bracket player won.
    fn next(&self, node: usize, source: fn(usize) -> Source) -> Option<&str> {
        self.plan
            .iter()
            .find(|later| later.sources.contains(&source(node)))
            .filter(|later| later.reset_of != Some(node) || self.states[node].winner == Some(1))
            .map(|later| later.label.as_str())
    }
}

fn encode(slot: Slot) -> Option<i64> {
    match slot {
        Slot::Open => None,
        Slot::Empty => Some(-1),
        Slot::Entrant(seed) => Some(seed as i64),
    }
}

fn decode(value: Option<i64>) -> Slot {
    match value {
        None => Slot::Open,
        Some(seed) if seed >= 0 => Slot::Entrant(seed as usize),
        Some(_) => Slot::Empty,
    }
}

fn load_bracket(tx: &Transaction<'_>, tournament: &Tournament) -> Result<Bracket> {
    let seeds: Vec<Entrant> = entrants(tx, &tournament.id)?
        .into_iter()
        .filter(|entrant| entrant.seed.is_some())
        .collect();
    let plan = bracket::plan(tournament.format, seeds.len(), tournament.grand_final_reset)
        .map_err(|_| ApiFailure::unavailable())?;
    let mut states = vec![NodeState::default(); plan.len()];
    let mut matches = vec![None; plan.len()];
    let mut counts = vec![0; plan.len()];
    let rows = tx
        .prepare(
            "SELECT node, slot_a, slot_b, status, winner_slot, match_id, match_count FROM tournament_nodes
             WHERE tournament_id = ?1",
        )?
        .query_map([&tournament.id], |row| {
            Ok((
                row.get::<_, usize>(0)?,
                row.get::<_, Option<i64>>(1)?,
                row.get::<_, Option<i64>>(2)?,
                row.get::<_, String>(3)?,
                row.get::<_, Option<u8>>(4)?,
                row.get::<_, Option<String>>(5)?,
                row.get::<_, u32>(6)?,
            ))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for (node, a, b, status, winner, match_id, count) in rows {
        if node >= plan.len() {
            return Err(ApiFailure::unavailable());
        }
        states[node] = NodeState {
            slots: [decode(a), decode(b)],
            status: Status::parse(&status).ok_or_else(ApiFailure::unavailable)?,
            winner,
        };
        matches[node] = match_id;
        counts[node] = count;
    }
    Ok(Bracket {
        plan,
        states,
        matches,
        counts,
        seeds,
    })
}

fn save(
    tx: &Transaction<'_>,
    tournament_id: &str,
    bracket: &Bracket,
    nodes: impl IntoIterator<Item = usize>,
    now: u64,
) -> Result<()> {
    for node in nodes {
        let state = &bracket.states[node];
        tx.execute(
            "UPDATE tournament_nodes SET slot_a = ?3, slot_b = ?4, status = ?5, winner_slot = ?6, match_id = ?7,
                match_count = ?8, updated_at = ?9
             WHERE tournament_id = ?1 AND node = ?2",
            params![
                tournament_id,
                node,
                encode(state.slots[0]),
                encode(state.slots[1]),
                state.status.as_str(),
                state.winner,
                bracket.matches[node],
                bracket.counts[node],
                now
            ],
        )?;
    }
    Ok(())
}

fn visible(tx: &Transaction<'_>, viewer: &Viewer, tournament: &Tournament) -> Result<bool> {
    Ok(match viewer {
        Viewer::Provider { connection_id, .. } => *connection_id == tournament.connection_id,
        Viewer::Organizer { tenant_id } => *tenant_id == tournament.tenant_id,
        Viewer::Player { ember_id } => tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM tournament_entrants WHERE tournament_id = ?1 AND ember_id = ?2)",
            params![tournament.id, ember_id.as_str()],
            |row| row.get(0),
        )?,
    })
}

fn touch(tx: &Transaction<'_>, tournament_id: &str, now: u64) -> Result<u64> {
    Ok(tx.query_row(
        "UPDATE tournaments SET revision = revision + 1, updated_at = ?2 WHERE id = ?1 RETURNING revision",
        params![tournament_id, now],
        |row| row.get(0),
    )?)
}

fn tournament_event(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament: &Tournament,
    kind: Kind,
    mut data: serde_json::Value,
    revision: u64,
) -> Result<i64> {
    data["tournament_id"] = tournament.id.clone().into();
    data["tournament_revision"] = revision.to_string().into();
    emit(
        tx,
        &ctx.config,
        ctx.now,
        NewEvent {
            kind,
            tenant_id: &tournament.tenant_id,
            connection_id: Some(&tournament.connection_id),
            subject: format!("tournaments/{}", tournament.id),
            match_id: None,
            ember_id: None,
            lobby_id: None,
            tournament_id: Some(&tournament.id),
            data,
        },
    )
}

fn set_json(bracket: &Bracket, node: usize) -> serde_json::Value {
    let plan = &bracket.plan[node];
    json!({
        "node": node,
        "side": plan.side.as_str(),
        "round": plan.round,
        "label": plan.label,
    })
}

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
fn progress(
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

/// One round robin entrant's results. A walkover counts as a set won or lost
/// with no games.
struct TableRow {
    seed: usize,
    sets_won: u32,
    sets_lost: u32,
    games_won: u32,
    games_lost: u32,
}

/// Sets won, then game difference, then games won, then the head-to-head
/// result when exactly two are level, then seed.
fn round_robin_table(tx: &Transaction<'_>, bracket: &Bracket) -> Result<Vec<TableRow>> {
    let mut rows: Vec<TableRow> = (0..bracket.seeds.len())
        .map(|seed| TableRow {
            seed,
            sets_won: 0,
            sets_lost: 0,
            games_won: 0,
            games_lost: 0,
        })
        .collect();
    let mut beat = BTreeSet::new();
    for (node, state) in bracket.states.iter().enumerate() {
        let (Some(winner), Some(loser)) = (state.winner(), state.loser()) else {
            continue;
        };
        if !matches!(state.status, Status::Played | Status::Walkover) {
            continue;
        }
        rows[winner].sets_won += 1;
        rows[loser].sets_lost += 1;
        beat.insert((winner, loser));
        if state.status == Status::Played
            && let Some(match_id) = &bracket.matches[node]
        {
            let (wins, _) = matches::scores(tx, match_id)?;
            for slot in 0..2 {
                if let Some(seed) = state.slots[slot].entrant() {
                    rows[seed].games_won += u32::from(wins[slot]);
                    rows[seed].games_lost += u32::from(wins[1 - slot]);
                }
            }
        }
    }
    let key = |row: &TableRow| {
        (
            std::cmp::Reverse(row.sets_won),
            std::cmp::Reverse(i64::from(row.games_won) - i64::from(row.games_lost)),
            std::cmp::Reverse(row.games_won),
        )
    };
    rows.sort_by(|a, b| key(a).cmp(&key(b)).then(a.seed.cmp(&b.seed)));
    // Two players level on everything above: the one who won their set goes first.
    let mut index = 0;
    while index < rows.len() {
        let mut end = index + 1;
        while end < rows.len() && key(&rows[end]) == key(&rows[index]) {
            end += 1;
        }
        if end - index == 2 && beat.contains(&(rows[index + 1].seed, rows[index].seed)) {
            rows.swap(index, index + 1);
        }
        index = end;
    }
    Ok(rows)
}

fn snapshot(tx: &Transaction<'_>, tournament: &Tournament) -> Result<serde_json::Value> {
    let all = entrants(tx, &tournament.id)?;
    let entrants_json: Vec<_> = all
        .iter()
        .map(|entrant| {
            json!({
                "ember_id": entrant.ember_id,
                "participant_id": entrant.participant_id,
                "state": if entrant.withdrawn { "withdrawn" } else { "registered" },
                "seed": entrant.seed.map(|seed| seed + 1),
                "placement": entrant.placement,
            })
        })
        .collect();
    let started = matches!(tournament.state.as_str(), "running" | "completed")
        || (tournament.state == "cancelled" && all.iter().any(|entrant| entrant.seed.is_some()));
    let (sets, standings) = if started {
        let bracket = load_bracket(tx, tournament)?;
        let mut sets = Vec::new();
        for (node, state) in bracket.states.iter().enumerate() {
            let mut set = set_json(&bracket, node);
            set["status"] = state.status.as_str().into();
            set["players"] = json!(state.slots.map(|slot| bracket.slot(slot, false)));
            set["winner_slot"] = state.winner.into();
            set["match_id"] = bracket.matches[node].clone().into();
            set["games_to_win"] = if bracket.plan[node].finals {
                tournament.finals_games_to_win
            } else {
                tournament.games_to_win
            }
            .into();
            if let Some(match_id) = &bracket.matches[node]
                && matches!(state.status, Status::Playing | Status::Played)
            {
                set["wins"] = json!(matches::scores(tx, match_id)?.0);
            }
            sets.push(set);
        }
        let standings = if tournament.format == Format::RoundRobin {
            round_robin_table(tx, &bracket)?
                .into_iter()
                .map(|row| {
                    let mut player = bracket.seeds[row.seed].player();
                    player["sets_won"] = row.sets_won.into();
                    player["sets_lost"] = row.sets_lost.into();
                    player["games_won"] = row.games_won.into();
                    player["games_lost"] = row.games_lost.into();
                    player
                })
                .collect()
        } else {
            Vec::new()
        };
        (sets, standings)
    } else {
        (Vec::new(), Vec::new())
    };
    Ok(json!({
        "tournament_id": tournament.id,
        "external_tournament_id": tournament.external_tournament_id,
        "state": tournament.state,
        "revision": tournament.revision.to_string(),
        "format": tournament.format,
        "games_to_win": tournament.games_to_win,
        "finals_games_to_win": tournament.finals_games_to_win,
        "grand_final_reset": tournament.grand_final_reset,
        "required_build_id": tournament.required_build_id,
        "metadata": tournament.metadata,
        "entrants": entrants_json,
        "sets": sets,
        "standings": standings,
        "event_cursor": events::head(tx)?.to_string(),
    }))
}

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

fn registered_count(tx: &Transaction<'_>, tournament_id: &str) -> Result<i64> {
    Ok(tx.query_row(
        "SELECT COUNT(*) FROM tournament_entrants WHERE tournament_id = ?1 AND state = 'registered'",
        [tournament_id],
        |row| row.get(0),
    )?)
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

fn closed() -> ApiFailure {
    ApiFailure::new(
        ErrorCode::StaleRevision,
        "The tournament is over or cancelled.",
    )
}

fn stale(current: u64) -> ApiFailure {
    ApiFailure::new(
        ErrorCode::StaleRevision,
        "The tournament changed. Refresh it and try again.",
    )
    .detail("current_revision", current.to_string())
}

fn check_reason(reason: &str) -> Result<()> {
    if reason.trim().is_empty() || reason.len() > 512 || reason.chars().any(char::is_control) {
        return Err(ApiFailure::invalid(
            "A reason of 1 to 512 bytes is required.",
        ));
    }
    Ok(())
}

fn actor(role: Role) -> &'static str {
    match role {
        Role::Provider => "service",
        Role::Organizer => "organizer",
    }
}

/// `POST /v1/tournaments`: a provider opens registration on its connection.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let key = idempotency_key(&headers)?;
    let command: CreateTournament = body.parse()?;
    command.check()?;
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let tenant_id = service.tenant_id.clone();
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, "/v1/tournaments", &key, &digest, ctx.now, |tx| {
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
                    return Ok((StatusCode::OK, snapshot(tx, &tournament)?));
                }
                let id = crate::util::new_id("etn");
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
                    &ctx,
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
                Ok((StatusCode::CREATED, snapshot(tx, &tournament)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}

/// `GET /v1/tournaments/{id}`: its provider, an organizer of its tenant, or
/// one of its entrants.
pub async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let viewer = viewer_of(&auth::any(&state, &headers).await?)?;
    let body = state
        .db
        .read(move |tx| {
            let tournament = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &tournament)? {
                return Err(ApiFailure::not_found());
            }
            let mut body = snapshot(tx, &tournament)?;
            if let Viewer::Player { ember_id } = &viewer {
                records::redact(&mut body, ember_id);
            }
            Ok(body)
        })
        .await?;
    Ok(ok(&body))
}

/// `POST /v1/tournaments/{id}/entrants`: the provider registers a linked
/// player who asked to enter, before the tournament starts. Registering again
/// changes nothing.
pub async fn register(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let key = idempotency_key(&headers)?;
    let command: RegisterEntrant = body.parse()?;
    command.check()?;
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/tournaments/{id}/entrants");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let tournament = load(tx, &id)?
                    .filter(|tournament| tournament.connection_id == connection_id)
                    .ok_or_else(ApiFailure::not_found)?;
                if tournament.state != "registration" {
                    return Err(ApiFailure::new(ErrorCode::StaleRevision, "Registration for this tournament is closed."));
                }
                if !matches::linked(tx, &connection_id, &command.participant_id, &command.ember_id)? {
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
                    return Ok((StatusCode::OK, snapshot(tx, &tournament)?));
                }
                let count = registered_count(tx, &tournament.id)?;
                let max = tournament.format.max_entrants() as i64;
                if count >= max {
                    return Err(ApiFailure::new(ErrorCode::RateLimited, "The tournament is full.").detail("max_entrants", max));
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
                    &ctx,
                    &tournament,
                    Kind::TournamentEntrantsChanged,
                    json!({ "reason": "registered", "ember_id": command.ember_id, "entrants": count + 1 }),
                    revision,
                )?;
                let tournament = load(tx, &tournament.id)?.ok_or_else(ApiFailure::unavailable)?;
                Ok((StatusCode::CREATED, snapshot(tx, &tournament)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct WithdrawCommand {
    #[serde(default)]
    expected_revision: Option<Counter>,
}

/// `POST /v1/tournaments/{id}/entrants/{participant_id}/withdraw`: its
/// provider or an organizer of its tenant, at the player's request or for a
/// no-show. Once running, every set they have left goes to their opponent.
pub async fn withdraw_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((id, participant)): Path<(String, String)>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let viewer = service_viewer(&service)?;
    let key = idempotency_key(&headers)?;
    let command: WithdrawCommand = if body.0.is_empty() {
        WithdrawCommand {
            expected_revision: None,
        }
    } else {
        body.parse()?
    };
    if !is_prefixed_id(&participant, "epl") {
        return Err(ApiFailure::invalid("Name the participant_id to withdraw."));
    }
    let expected = command.expected_revision.map(|revision| revision.0);
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/tournaments/{id}/entrants/{participant}/withdraw");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let tournament = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
                if !visible(tx, &viewer, &tournament)? {
                    return Err(ApiFailure::not_found());
                }
                if expected.is_some_and(|expected| expected != tournament.revision) {
                    return Err(stale(tournament.revision));
                }
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
                    withdraw(tx, &ctx, &tournament, &ember_id, "withdrew")?;
                    audit(tx, ctx.now, (actor(service.role), service.id.clone()), "tournament.withdraw", &tournament.id, "ok", None)?;
                }
                let tournament = load(tx, &tournament.id)?.ok_or_else(ApiFailure::unavailable)?;
                Ok((StatusCode::OK, snapshot(tx, &tournament)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct StartCommand {
    #[serde(default)]
    expected_revision: Option<Counter>,
    /// Every registered `participant_id`, top seed first. Registration order
    /// when absent.
    #[serde(default)]
    seeding: Option<Vec<String>>,
}

/// `POST /v1/tournaments/{id}/start`: its provider or an organizer of its
/// tenant closes registration, seeds the entrants and starts the first sets.
pub async fn start(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let viewer = service_viewer(&service)?;
    let key = idempotency_key(&headers)?;
    let command: StartCommand = body.parse()?;
    let expected = expected_revision(
        &headers,
        command.expected_revision.map(|revision| revision.0),
    )?;
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/tournaments/{id}/start");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let tournament = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
                if !visible(tx, &viewer, &tournament)? {
                    return Err(ApiFailure::not_found());
                }
                if tournament.revision != expected {
                    return Err(stale(tournament.revision));
                }
                if tournament.state != "registration" {
                    return Err(ApiFailure::new(ErrorCode::StaleRevision, "The tournament has already started."));
                }
                let registered: Vec<Entrant> = entrants(tx, &tournament.id)?
                    .into_iter()
                    .filter(|entrant| !entrant.withdrawn)
                    .collect();
                if registered.len() < 2 {
                    return Err(ApiFailure::invalid("A tournament needs at least two entrants."));
                }
                let order: Vec<&Entrant> = match &command.seeding {
                    None => registered.iter().collect(),
                    Some(seeding) => {
                        let mut seen = BTreeSet::new();
                        let order: Vec<&Entrant> = seeding
                            .iter()
                            .filter(|participant| seen.insert(participant.as_str()))
                            .filter_map(|participant| registered.iter().find(|entrant| entrant.participant_id == *participant))
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
                tx.execute("UPDATE tournaments SET state = 'running' WHERE id = ?1", [&tournament.id])?;
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
                    &ctx,
                    &tournament,
                    Kind::TournamentStarted,
                    json!({ "format": tournament.format, "entrants": seeds, "sets": plan.len() }),
                    revision,
                )?;
                audit(tx, ctx.now, (actor(service.role), service.id.clone()), "tournament.start", &tournament.id, "ok", None)?;
                let mut bracket = load_bracket(tx, &tournament)?;
                progress(tx, &ctx, &tournament, &mut bracket)?;
                let tournament = load(tx, &tournament.id)?.ok_or_else(ApiFailure::unavailable)?;
                Ok((StatusCode::OK, snapshot(tx, &tournament)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct CancelCommand {
    reason: String,
    #[serde(default)]
    expected_revision: Option<Counter>,
}

/// `POST /v1/tournaments/{id}/cancel`: its provider or an organizer of its
/// tenant. Sets being played have their matches cancelled.
pub async fn cancel(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let viewer = service_viewer(&service)?;
    let key = idempotency_key(&headers)?;
    let command: CancelCommand = body.parse()?;
    check_reason(&command.reason)?;
    let expected = expected_revision(
        &headers,
        command.expected_revision.map(|revision| revision.0),
    )?;
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/tournaments/{id}/cancel");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let tournament = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
                if !visible(tx, &viewer, &tournament)? {
                    return Err(ApiFailure::not_found());
                }
                if tournament.revision != expected {
                    return Err(stale(tournament.revision));
                }
                if !matches!(tournament.state.as_str(), "registration" | "running") {
                    return Err(closed());
                }
                let mut freed = Vec::new();
                if tournament.state == "running" {
                    let bracket = load_bracket(tx, &tournament)?;
                    for (node, state) in bracket.states.iter().enumerate() {
                        if state.status == Status::Playing
                            && let Some(match_id) = &bracket.matches[node]
                        {
                            freed.extend(matches::cancel_tournament_set(
                                tx,
                                &ctx,
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
                    &ctx,
                    &tournament,
                    Kind::TournamentCancelled,
                    json!({ "reason": command.reason }),
                    revision,
                )?;
                audit(
                    tx,
                    ctx.now,
                    (actor(service.role), service.id.clone()),
                    "tournament.cancel",
                    &tournament.id,
                    "ok",
                    Some(&command.reason),
                )?;
                matches::release_players(tx, &ctx, &tournament.connection_id, &freed)?;
                // Entrants a ready set held back from lobbies are free now.
                let everyone: Vec<EmberId> = entrants(tx, &tournament.id)?
                    .into_iter()
                    .map(|entrant| entrant.ember_id)
                    .collect();
                lobbies::on_players_free(tx, &ctx, &tournament.connection_id, &everyone)?;
                let tournament = load(tx, &tournament.id)?.ok_or_else(ApiFailure::unavailable)?;
                Ok((StatusCode::OK, snapshot(tx, &tournament)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}
