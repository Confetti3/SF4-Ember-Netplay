//! Round robin standings.
use std::collections::BTreeSet;

use ember_protocol::tournament::Status;
use rusqlite::Transaction;

use super::state::Bracket;
use crate::{error::Result, routes::matches};

/// One round robin entrant's results. A walkover counts as a set won or lost
/// with no games.
pub struct TableRow {
    pub seed: usize,
    pub sets_won: u32,
    pub sets_lost: u32,
    pub games_won: u32,
    pub games_lost: u32,
}

/// Sets won, then game difference, then games won, then the head-to-head
/// result when exactly two are level, then seed.
pub fn round_robin_table(tx: &Transaction<'_>, bracket: &Bracket) -> Result<Vec<TableRow>> {
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
