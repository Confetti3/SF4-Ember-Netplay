//! Which of a match's watchers its binding names. `match_watchers` is the
//! source: every watcher who asked and is still asking. The bound set is
//! derived from it and the match's watch policy, never edited in place, and
//! every fighter claim and watcher call re-derives it (`play::rebind` calls
//! `named`), so a revocation, a policy change or a watcher who went quiet
//! reaches the room within a claim.
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
    routes::play::{ClaimRow, PlayedMatch},
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

/// A watcher whose last ask is not newer than this has gone quiet.
pub fn live_after(now: u64) -> u64 {
    now.saturating_sub(LIVE_SECS)
}

/// The rows of `match_id` last seen after `after` (all of them for `None`),
/// oldest arrival first.
pub fn rows(tx: &Transaction<'_>, match_id: &str, after: Option<u64>) -> Result<Vec<Row>> {
    tx.prepare(
        "SELECT ember_id, endpoint_id, build_id, first_seen_at, last_seen_at FROM match_watchers
         WHERE match_id = ?1 AND (?2 IS NULL OR last_seen_at > ?2) ORDER BY first_seen_at, ember_id",
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

/// The watchers a binding names, from the text `named` stores.
pub fn bound(text: Option<&str>) -> Result<Vec<BoundWatcher>> {
    text.map_or_else(
        || Ok(vec![]),
        |text| serde_json::from_str(text).map_err(|_| ApiFailure::unavailable()),
    )
}

/// The claims of the match's two fighters, leaving out any left by an Ember ID
/// that has since been reassigned.
pub fn fighter_claims<'a>(played: &PlayedMatch, claimed: &'a [ClaimRow]) -> Vec<&'a ClaimRow> {
    claimed
        .iter()
        .filter(|claim| played.roster.iter().any(|p| p.ember_id == claim.ember_id))
        .collect()
}

/// Whether both fighters' latest claims say their helpers read watchers.
pub fn attested(fighters: &[&ClaimRow]) -> bool {
    fighters.len() == 2 && fighters.iter().all(|claim| claim.watch)
}

/// Two watchers asking for one endpoint: the lower-ranked is dropped.
struct Conflict {
    loser: EmberId,
    winner: EmberId,
    endpoint_id: String,
}

/// The watchers the match may name, oldest first, and the endpoint conflicts
/// that kept some out. `fighters` are the two fighters' claims.
fn select(
    rows: Vec<Row>,
    policy: &Policy,
    played: &PlayedMatch,
    seated: &[String; 2],
    fighters: &[&ClaimRow],
) -> (Vec<Row>, Vec<Conflict>) {
    let mut candidates: Vec<(Admission, Row)> = rows
        .into_iter()
        .map(|row| (policy.admission(&row.ember_id), row))
        .filter(|(admission, row)| {
            matches!(admission, Admission::Staff | Admission::LinkHolder)
                && played.roster.iter().all(|p| p.ember_id != row.ember_id)
                && !seated.contains(&row.endpoint_id)
                && fighters.iter().all(|claim| claim.build_id == row.build_id)
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

/// The text `match_rooms.bound_watchers` should hold: the watchers the
/// binding names, or `None` for none.
///
/// A fighter's helper reads `watchers` only if its own latest claim says so
/// (`Claim::watch`), so the binding names nobody unless both fighters' claims
/// do. A watcher's `build_id` is self-declared and vouches for nothing: it
/// only has to equal the fighters' build. `seated` is the fighters' endpoints,
/// once the room is published and both have claimed.
pub fn named(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    played: &PlayedMatch,
    claimed: &[ClaimRow],
    seated: Option<&[String; 2]>,
) -> Result<Option<String>> {
    let Some(seated) = seated else {
        return Ok(None);
    };
    let match_id = &played.found.id;
    let asked = rows(tx, match_id, Some(live_after(ctx.now)))?;
    let fighters = fighter_claims(played, claimed);
    if asked.is_empty() || !attested(&fighters) {
        return Ok(None);
    }
    let policy = Policy::load(tx, &played.found.connection_id, match_id)?;
    let (chosen, conflicts) = select(asked, &policy, played, seated, &fighters);
    for conflict in &conflicts {
        // Once per pair and endpoint, however often they keep asking.
        let fresh = tx.execute(
            "INSERT OR IGNORE INTO match_watch_conflicts (match_id, loser, winner, endpoint_id) VALUES (?1, ?2, ?3, ?4)",
            params![
                match_id,
                conflict.loser.as_str(),
                conflict.winner.as_str(),
                conflict.endpoint_id
            ],
        )?;
        if fresh == 1 {
            let reason = format!(
                "{} lost endpoint {} to {}",
                conflict.loser, conflict.endpoint_id, conflict.winner
            );
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
    let named: Vec<BoundWatcher> = chosen
        .into_iter()
        .map(|row| BoundWatcher {
            ember_id: row.ember_id,
            endpoint_id: row.endpoint_id,
        })
        .collect();
    Ok((!named.is_empty()).then(|| serde_json::to_string(&named).unwrap_or_default()))
}
