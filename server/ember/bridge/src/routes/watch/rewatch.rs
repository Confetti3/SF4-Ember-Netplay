//! Which of a match's watchers its binding names. `match_watchers` is the
//! source: every watcher who asked and is still asking. The bound set is
//! derived from it and the match's watch policy, never edited in place, and
//! every fighter claim and watcher call re-derives it, so a revocation, a
//! policy change or a watcher who went quiet reaches the room within a claim.
use std::collections::BTreeMap;

use ember_protocol::{
    EmberId,
    play::{BoundWatcher, MAX_WATCHERS},
};
use rusqlite::{Transaction, params};

use super::access::{Admission, Policy};
use crate::{
    audit::audit,
    ctx::Ctx,
    error::{ApiFailure, Result},
    routes::play::{ClaimRow, Fighter, endpoints, room},
};

/// Watchers who hold the match link and nothing more, at most.
pub const LINK_HOLDER_CAP: usize = 4;
/// A watcher stays in the set this long after it last asked.
pub const LIVE_SECS: u64 = 60;

/// A watcher who asked to watch.
pub struct Row {
    pub ember_id: EmberId,
    pub endpoint_id: String,
    pub build_id: String,
    pub first_seen_at: u64,
    pub last_seen_at: u64,
}

/// The rows of `match_id` seen after `after`, oldest arrival first.
pub fn rows(tx: &Transaction<'_>, match_id: &str, after: i64) -> Result<Vec<Row>> {
    tx.prepare(
        "SELECT ember_id, endpoint_id, build_id, first_seen_at, last_seen_at FROM match_watchers
         WHERE match_id = ?1 AND last_seen_at > ?2 ORDER BY first_seen_at, ember_id",
    )?
    .query_map(params![match_id, after], |row| {
        Ok((
            row.get::<_, String>(0)?,
            row.get(1)?,
            row.get(2)?,
            row.get(3)?,
            row.get(4)?,
        ))
    })?
    .map(|row| {
        let (ember_id, endpoint_id, build_id, first_seen_at, last_seen_at) = row?;
        Ok(Row {
            ember_id: EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?,
            endpoint_id,
            build_id,
            first_seen_at,
            last_seen_at,
        })
    })
    .collect()
}

/// The watchers a binding names, from the text `rewatch` stores.
pub fn bound(text: Option<&str>) -> Result<Vec<BoundWatcher>> {
    let Some(text) = text else {
        return Ok(vec![]);
    };
    let pairs: Vec<(String, String)> =
        serde_json::from_str(text).map_err(|_| ApiFailure::unavailable())?;
    pairs
        .into_iter()
        .map(|(ember_id, endpoint_id)| {
            Ok(BoundWatcher {
                ember_id: EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?,
                endpoint_id,
            })
        })
        .collect()
}

/// Two watchers asking for one endpoint: the lower-ranked is dropped.
struct Conflict {
    loser: EmberId,
    winner: EmberId,
    endpoint_id: String,
}

/// The watchers the match may name, oldest first, and the endpoint conflicts
/// that kept some out.
fn select(
    rows: Vec<Row>,
    policy: &Policy,
    fighter: &Fighter,
    seated: &[String; 2],
    builds: &[ClaimRow],
) -> (Vec<Row>, Vec<Conflict>) {
    let mut candidates: Vec<(Admission, Row)> = rows
        .into_iter()
        .map(|row| (policy.admission(&row.ember_id), row))
        .filter(|(admission, row)| {
            matches!(admission, Admission::Staff | Admission::LinkHolder)
                && fighter.roster.iter().all(|p| p.ember_id != row.ember_id)
                && !seated.contains(&row.endpoint_id)
                && builds.iter().all(|claim| claim.build_id == row.build_id)
        })
        .collect();
    // Staff beat link holders, and the latest arrival beats an earlier one.
    // Arrival is `first_seen_at`, which polling does not move, so two
    // watchers who ask for one endpoint do not trade it back and forth.
    candidates.sort_by(|(a_via, a), (b_via, b)| {
        a.endpoint_id
            .cmp(&b.endpoint_id)
            .then(a_via.cmp(b_via))
            .then(b.first_seen_at.cmp(&a.first_seen_at))
            .then(a.ember_id.as_str().cmp(b.ember_id.as_str()))
    });
    let mut owners: BTreeMap<String, EmberId> = BTreeMap::new();
    let mut conflicts = vec![];
    let mut kept = vec![];
    for (admission, row) in candidates {
        match owners.get(&row.endpoint_id) {
            Some(winner) => conflicts.push(Conflict {
                loser: row.ember_id,
                winner: winner.clone(),
                endpoint_id: row.endpoint_id,
            }),
            None => {
                owners.insert(row.endpoint_id.clone(), row.ember_id.clone());
                kept.push((admission, row));
            }
        }
    }
    // Staff first, then link holders up to their cap, up to the ceiling; each
    // group oldest first.
    kept.sort_by(|(a_via, a), (b_via, b)| {
        a_via
            .cmp(b_via)
            .then(a.first_seen_at.cmp(&b.first_seen_at))
            .then(a.ember_id.as_str().cmp(b.ember_id.as_str()))
    });
    let mut link_holders = 0;
    let mut chosen: Vec<Row> = kept
        .into_iter()
        .filter(|(admission, _)| {
            if *admission == Admission::LinkHolder {
                link_holders += 1;
                return link_holders <= LINK_HOLDER_CAP;
            }
            true
        })
        .map(|(_, row)| row)
        .take(MAX_WATCHERS)
        .collect();
    chosen.sort_by(|a, b| {
        (a.first_seen_at, a.ember_id.as_str()).cmp(&(b.first_seen_at, b.ember_id.as_str()))
    });
    (chosen, conflicts)
}

/// Derives the watchers the match's binding names, and moves the binding
/// revision, but not the fighters' revision, when they changed. A match nobody
/// ever asked to watch is never touched.
pub fn rewatch(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    fighter: &Fighter,
    claimed: &[ClaimRow],
) -> Result<()> {
    let match_id = &fighter.found.id;
    let current = room(tx, match_id)?;
    let asked = rows(
        tx,
        match_id,
        ctx.now.saturating_sub(LIVE_SECS).try_into().unwrap_or(0),
    )?;
    if asked.is_empty() && current.bound_watchers.is_none() {
        return Ok(());
    }
    let (chosen, conflicts) = match endpoints(fighter, claimed) {
        Some(seated) if current.room_id.is_some() => {
            let policy = Policy::load(tx, &fighter.found.connection_id, match_id)?;
            select(asked, &policy, fighter, &seated, claimed)
        }
        _ => (vec![], vec![]),
    };
    for conflict in &conflicts {
        let reason = format!(
            "{} lost endpoint {} to {}",
            conflict.loser, conflict.endpoint_id, conflict.winner
        );
        let known: bool = tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM audit_entries WHERE action = 'watch.endpoint_conflict' AND target = ?1 AND reason = ?2)",
            params![match_id, reason],
            |row| row.get(0),
        )?;
        if !known {
            audit(
                tx,
                ctx.now,
                ("bridge", "watch".into()),
                "watch.endpoint_conflict",
                match_id,
                "ok",
                Some(&reason),
            )?;
        }
    }
    let text = (!chosen.is_empty()).then(|| {
        let pairs: Vec<_> = chosen
            .iter()
            .map(|row| (row.ember_id.as_str(), row.endpoint_id.as_str()))
            .collect();
        serde_json::to_string(&pairs).unwrap_or_default()
    });
    if text != current.bound_watchers {
        tx.execute(
            "UPDATE match_rooms SET binding_revision = binding_revision + 1, bound_watchers = ?1 WHERE match_id = ?2",
            params![text, match_id],
        )?;
    }
    Ok(())
}
