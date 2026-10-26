//! BluMint's status report on a match (`routes::blumint::status`): whether
//! its fighters' first game has started, and each player's presence. It is
//! observed whenever the match's revision moves, whenever a fighter claims
//! it and whenever a fighter's claim lapses, and sent each time it changes:
//! as each player arrives or leaves, as each game starts, and as each game
//! ends and the next waits.
//!
//! A match keeps one report, its latest. Each change moves the report's
//! `revision` and queues it again, so a report that waited is sent as it
//! stands, and BluMint drops one that reaches it after a later one. Nothing
//! is sent once the match has ended: its result or abort says how it ended.
use std::collections::BTreeMap;

use rusqlite::{OptionalExtension, Transaction, params};
use serde_json::{Value, json};

use super::{BATCH, Leased, Outcome, Queue, Record, Settled, results::BluMint};
use crate::{
    AppState,
    audit::audit,
    db,
    error::{ApiFailure, Result},
    routes::{
        blumint::{self, liveness},
        ledger::{load, load_listed},
        policy,
    },
};

/// The longest a writer's clock read can trail an earlier commit (`observe`).
const MAX_COMMIT_WAIT_SECS: u64 = 10;

/// Observes the match's status, and queues it to be sent when it differs
/// from the one last observed. A match nobody has opened and no game has
/// started on has nothing to say yet.
pub fn observe(tx: &Transaction<'_>, match_id: &str, now: u64) -> Result<()> {
    let found = load(tx, match_id)?.ok_or_else(ApiFailure::unavailable)?;
    if found.state.is_terminal() || !policy::of(tx, &found.connection_id)?.reports_progress {
        return Ok(());
    }
    let last: Option<(String, u64)> = tx
        .query_row(
            "SELECT body, observed_at FROM blumint_status WHERE match_id = ?1",
            [match_id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?;
    // A writer reads the clock before it waits for the database, so one
    // that commits later can carry an earlier time. The status is never
    // observed before the last one was, or a claim seen lapsed would read
    // live again. A last observation further ahead than any such wait is the
    // clock stepping back, and is not followed, or every claim would read
    // lapsed until the clock caught up.
    let now = match &last {
        Some((_, at)) if *at > now + MAX_COMMIT_WAIT_SECS => {
            eprintln!(
                "status: {match_id} was last observed at {at}, {}s after now; the clock went back",
                at - now
            );
            now
        }
        Some((_, at)) => now.max(*at),
        None => now,
    };
    let status = blumint::status(tx, &found, now)?;
    // A stored body the bridge cannot read is written again from the status
    // as it stands, so it never stops the match's own writes.
    let last = match last {
        Some((text, _)) => serde_json::from_str(&text)
            .inspect_err(|_| {
                eprintln!("status: {match_id}'s stored status does not parse; writing it again")
            })
            .ok(),
        None => Some(blumint::unopened_status(tx, &found)?),
    };
    if last.as_ref() == Some(&status) {
        return Ok(());
    }
    // `attempts` restarts with the revision: a lease of an earlier revision
    // is told apart by its revision, not its attempt.
    tx.execute(
        "INSERT INTO blumint_status (match_id, revision, body, observed_at, state) VALUES (?1, 1, ?2, ?3, 'queued')
         ON CONFLICT (match_id) DO UPDATE SET revision = revision + 1, body = ?2, observed_at = ?3,
             state = 'queued', next_at = 0, attempts = 0, first_at = NULL",
        params![match_id, status.to_string(), now],
    )?;
    db::queued();
    Ok(())
}

/// Observes each open match a fighter's claim has lapsed on since the last
/// pass: nothing else happens when an Ember stops claiming, so the
/// maintenance pass notices it leave. A match whose status already says so
/// is unchanged.
pub fn observe_lapsed(tx: &Transaction<'_>, now: u64) -> Result<()> {
    for id in liveness::sweep(tx, now)? {
        // A match whose stored state this bridge does not know is passed over.
        if load_listed(tx, &id)?.is_some() {
            observe(tx, &id, now)?;
        }
    }
    Ok(())
}

/// Records that the bridge starts again at `now`, before it serves. It first
/// runs the pass it missed at the last moment it ran, so a claim that lapsed
/// after the last pass is swept as it was then, and is never extended.
pub fn resume(tx: &Transaction<'_>, now: u64) -> Result<()> {
    let Some(last) = liveness::last_alive(tx, now)? else {
        return Ok(());
    };
    observe_lapsed(tx, last)?;
    liveness::resume(tx, last, now)
}

/// The status queue (a `delivery::Queue`).
pub struct Status {
    destinations: BTreeMap<String, BluMint>,
}

impl Status {
    pub fn new(state: &AppState) -> Self {
        Self {
            destinations: BluMint::destinations(state),
        }
    }
}

/// A revision of a match's status, as it was leased.
pub struct Due {
    match_id: String,
    connection_id: String,
    revision: u64,
    body: Value,
}

impl Queue for Status {
    type Item = Due;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<Due>>> {
        tx.execute(
            "UPDATE blumint_status SET state = 'not_required'
             WHERE state IN ('queued', 'retrying', 'delivering')
               AND EXISTS (SELECT 1 FROM matches m WHERE m.id = blumint_status.match_id
                             AND m.state IN ('completed', 'cancelled', 'failed', 'expired'))",
            [],
        )?;
        let connections: Vec<&String> = self.destinations.keys().collect();
        let rows = tx
            .prepare(
                "SELECT s.match_id, m.connection_id, s.revision, s.body, s.observed_at, s.attempts, s.first_at,
                        m.external_match_id
                 FROM blumint_status s JOIN matches m ON m.id = s.match_id
                   JOIN provider_connections c ON c.id = m.connection_id
                 WHERE s.state IN ('queued', 'retrying', 'delivering') AND s.next_at <= ?1
                   AND m.connection_id IN (SELECT value FROM json_each(?2)) AND c.enabled = 1
                 ORDER BY s.next_at, s.match_id LIMIT ?3",
            )?
            .query_map(params![now, json!(connections).to_string(), BATCH], |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, u64>(2)?,
                    row.get::<_, String>(3)?,
                    row.get::<_, u64>(4)?,
                    row.get::<_, u32>(5)?,
                    row.get::<_, Option<u64>>(6)?,
                    row.get::<_, String>(7)?,
                ))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        let mut leased = Vec::with_capacity(rows.len());
        for (match_id, connection_id, revision, body, observed_at, attempts, first_at, external) in
            rows
        {
            // A body the bridge cannot read fails on its own, so the rest of
            // the queue goes on. The match's next change writes it again
            // (`observe`).
            let Ok(status) = serde_json::from_str(&body) else {
                eprintln!("status: {match_id} revision {revision} does not parse; not sent");
                tx.execute(
                    "UPDATE blumint_status SET state = 'failed', next_at = ?1 WHERE match_id = ?2",
                    params![now, match_id],
                )?;
                audit(
                    tx,
                    now,
                    ("bridge", "blumint".into()),
                    "blumint.status",
                    &match_id,
                    "failed",
                    Some("the stored status does not parse"),
                )?;
                continue;
            };
            let attempt = attempts + 1;
            tx.execute(
                "UPDATE blumint_status SET state = 'delivering', next_at = ?1, attempts = ?2
                 WHERE match_id = ?3",
                params![until, attempt, match_id],
            )?;
            leased.push(Leased {
                item: Due {
                    body: blumint::status_report(
                        &match_id,
                        &external,
                        &status,
                        revision,
                        observed_at,
                    ),
                    match_id,
                    connection_id,
                    revision,
                },
                attempt,
                first_attempt_at: first_at,
            });
        }
        Ok(leased)
    }

    async fn attempt(&self, state: &AppState, item: &Due) -> Outcome {
        let Some(destination) = self.destinations.get(&item.connection_id) else {
            return Outcome::Cancelled;
        };
        let id = item.match_id.clone();
        let open = state
            .db
            .read(move |tx| match load(tx, &id)? {
                Some(found) if !found.state.is_terminal() => {
                    policy::enabled(tx, &found.connection_id)
                }
                _ => Ok(false),
            })
            .await;
        match open {
            // The match ended since the lease, and the next lease marks the
            // report not required; or its connection was disabled, and it is
            // sent once it is enabled again.
            Ok(false) => return Outcome::Cancelled,
            Ok(true) => {}
            Err(_) => {
                return Outcome::Retry {
                    error: "database unavailable".into(),
                    retry_after: None,
                };
            }
        }
        match destination.post(item.body.to_string().into_bytes()).await {
            // A 404 `unknown_match` is final, as it is for a result: BluMint
            // has no lobby under the match and says to stop without alerting.
            // A status sent before it recorded the create answers 503
            // `match_not_ready` instead, which is retried.
            Ok(answered) => BluMint::outcome(&answered),
            Err(outcome) => outcome,
        }
    }

    fn record(&self, tx: &Transaction<'_>, leased: &Leased<Due>, record: &Record) -> Result<()> {
        let (state, next, error) = match &record.settled {
            Settled::Delivered => ("delivered", record.at, None),
            Settled::Retry { at, error } => ("retrying", *at, Some(error.as_str())),
            Settled::Refused(error) | Settled::Expired(error) => {
                ("failed", record.at, Some(error.as_str()))
            }
        };
        let changed = tx.execute(
            "UPDATE blumint_status SET state = ?1, next_at = ?2, first_at = COALESCE(first_at, ?3)
             WHERE match_id = ?4 AND revision = ?5 AND state = 'delivering' AND attempts = ?6",
            params![
                state,
                next,
                record.first_attempt_at,
                leased.item.match_id,
                leased.item.revision,
                leased.attempt
            ],
        )?;
        if changed == 1 {
            audit(
                tx,
                record.at,
                ("bridge", "blumint".into()),
                "blumint.status",
                &leased.item.match_id,
                state,
                error,
            )?;
        }
        Ok(())
    }
}
