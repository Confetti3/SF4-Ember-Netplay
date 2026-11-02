//! Stores the replay archive's games in its bucket (`crate::archive`):
//! tournament games first, each as `games/<digest>.emberreplay`. A stored
//! game's replay leaves the bridge. Nothing is given up: the game marks a
//! replay sent once the bridge has it, so this is the archive's only copy,
//! and one the bucket did not take is tried again, hourly once a day has
//! passed. Sets' packs are stored by `packs`.
use std::sync::Arc;

use rusqlite::{Transaction, params};

use super::{BATCH, Leased, Outcome, Queue, Record, Settled};
use crate::{AppState, error::Result, s3::Bucket};

pub struct Due {
    digest: String,
    replay: Vec<u8>,
}

/// Games waiting for the archive's bucket.
pub struct Archive(pub Arc<Bucket>);

impl Queue for Archive {
    type Item = Due;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<Due>>> {
        let rows = tx
            .prepare(
                "SELECT digest, attempts, first_attempt_at, replay FROM archive_games
                 WHERE state = 'pending' AND next_attempt_at <= ?1
                 ORDER BY priority, next_attempt_at, received_at LIMIT ?2",
            )?
            .query_map(params![now, BATCH], |row| {
                Ok(Leased {
                    attempt: row.get::<_, u32>(1)? + 1,
                    first_attempt_at: row.get(2)?,
                    item: Due {
                        digest: row.get(0)?,
                        replay: row.get(3)?,
                    },
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        for row in &rows {
            tx.execute(
                "UPDATE archive_games SET next_attempt_at = ?1, attempts = ?2 WHERE digest = ?3",
                params![until, row.attempt, row.item.digest],
            )?;
        }
        Ok(rows)
    }

    async fn attempt(&self, state: &AppState, item: &Due) -> Outcome {
        let key = format!("games/{}.emberreplay", item.digest);
        store(state, &self.0, &key, item.replay.clone()).await
    }

    fn record(&self, tx: &Transaction<'_>, leased: &Leased<Due>, record: &Record) -> Result<()> {
        let (stored, next, error) = settled(record);
        if let Some(error) = error {
            eprintln!("archive: {} not stored yet: {error}", leased.item.digest);
        }
        let recorded = tx.execute(
            "UPDATE archive_games SET
                state = CASE WHEN ?1 THEN 'stored' ELSE state END,
                replay = CASE WHEN ?1 THEN NULL ELSE replay END,
                stored_at = CASE WHEN ?1 THEN ?2 ELSE stored_at END,
                next_attempt_at = ?3, last_error = ?4,
                first_attempt_at = CASE WHEN ?1 THEN first_attempt_at ELSE COALESCE(first_attempt_at, ?5) END
             WHERE digest = ?6 AND state = 'pending' AND attempts = ?7",
            params![
                stored,
                record.at,
                next,
                error,
                record.first_attempt_at,
                leased.item.digest,
                leased.attempt
            ],
        )?;
        if stored && recorded > 0 {
            crate::archive::add_choices(tx, &leased.item.digest)?;
        }
        Ok(())
    }
}

/// One attempt of either archive queue: `body` put at `key` in `bucket`.
pub(super) async fn store(state: &AppState, bucket: &Bucket, key: &str, body: Vec<u8>) -> Outcome {
    let Some(secret) = &state.integrations.archive_secret_key else {
        return retry("no archive secret key".into());
    };
    match bucket.put(secret, key, body, state.now()).await {
        Ok(()) => Outcome::Delivered,
        Err(error) => retry(error),
    }
}

/// Where an archive queue's attempt leaves its item: whether it is stored,
/// when it is next due and why not, if it is not. The archive never gives
/// up on what it holds, so a refusal or a day of retries is tried hourly.
pub(super) fn settled(record: &Record) -> (bool, u64, Option<&String>) {
    match &record.settled {
        Settled::Delivered => (true, record.at, None),
        Settled::Retry { at, error } => (false, *at, Some(error)),
        Settled::Refused(error) | Settled::Expired(error) => (false, record.at + 3600, Some(error)),
    }
}

pub(super) fn retry(error: String) -> Outcome {
    Outcome::Retry {
        error,
        retry_after: None,
    }
}
