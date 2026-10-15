//! The persisted bracket: entrants, and each node's slots, status and match.
use ember_protocol::{
    EmberId,
    tournament::{self as bracket, Node, NodeState, Slot, Source, Status},
};
use rusqlite::{Transaction, params};
use serde_json::json;

use super::Tournament;
use crate::error::{ApiFailure, Result};

#[derive(Clone, Debug)]
pub struct Entrant {
    pub ember_id: EmberId,
    pub participant_id: String,
    pub withdrawn: bool,
    pub seed: Option<usize>,
    pub placement: Option<u32>,
}

impl Entrant {
    /// For snapshots; a player reading one sees only their own `participant_id`.
    pub fn player(&self) -> serde_json::Value {
        json!({ "ember_id": self.ember_id, "participant_id": self.participant_id })
    }

    /// For events, which entrants read unchanged: no account handles.
    pub fn public(&self) -> serde_json::Value {
        json!({ "ember_id": self.ember_id })
    }
}

/// Seeded entrants first, by seed, then the rest in registration order.
pub fn entrants(tx: &Transaction<'_>, tournament_id: &str) -> Result<Vec<Entrant>> {
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

pub(super) fn registered_count(tx: &Transaction<'_>, tournament_id: &str) -> Result<i64> {
    Ok(tx.query_row(
        "SELECT COUNT(*) FROM tournament_entrants WHERE tournament_id = ?1 AND state = 'registered'",
        [tournament_id],
        |row| row.get(0),
    )?)
}

/// A running or finished bracket in memory.
pub struct Bracket {
    pub plan: Vec<Node>,
    pub states: Vec<NodeState>,
    pub matches: Vec<Option<String>>,
    pub counts: Vec<u32>,
    /// Entrants by seed.
    pub seeds: Vec<Entrant>,
}

impl Bracket {
    pub fn withdrawn(&self) -> Vec<bool> {
        self.seeds.iter().map(|entrant| entrant.withdrawn).collect()
    }

    pub fn slot(&self, slot: Slot, public: bool) -> serde_json::Value {
        match slot {
            Slot::Open => serde_json::Value::Null,
            Slot::Empty => json!({ "bye": true }),
            Slot::Entrant(seed) if public => self.seeds[seed].public(),
            Slot::Entrant(seed) => self.seeds[seed].player(),
        }
    }

    pub fn entrant(&self, seed: Option<usize>) -> Option<&Entrant> {
        seed.map(|seed| &self.seeds[seed])
    }

    /// The label of the set that a node's winner or loser plays next. A
    /// grand final reset counts only when the losers-bracket player won.
    pub fn next(&self, node: usize, source: fn(usize) -> Source) -> Option<&str> {
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

pub fn load_bracket(tx: &Transaction<'_>, tournament: &Tournament) -> Result<Bracket> {
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

pub fn save(
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

pub fn set_json(bracket: &Bracket, node: usize) -> serde_json::Value {
    let plan = &bracket.plan[node];
    json!({
        "node": node,
        "side": plan.side.as_str(),
        "round": plan.round,
        "label": plan.label,
    })
}
