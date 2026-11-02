//! The archive's games kept before it knew them by `Replay::game`
//! (`archive::duplicates`). `Read` reads each such copy back from the bucket
//! and records which game it is; `Remove` deletes from the bucket the copies
//! that folding left over, then their rows. A copy whose upload is still
//! leased (`next_attempt_at` ahead) is not deleted before that attempt is
//! over, so its file cannot land after the delete.
use std::sync::Arc;

use ember_protocol::replays::{MAX_REPLAY, Replay};
use rusqlite::{Transaction, params};

use super::{
    BATCH, Leased, Outcome, Queue, Record,
    archive::{retry, settled},
};
use crate::{AppState, archive::duplicates::identified, error::Result, s3::Bucket};

/// Stored games not yet known by `game`.
pub struct Read(pub Arc<Bucket>);

/// Copies folded into another, still in the bucket.
pub struct Remove(pub Arc<Bucket>);

impl Queue for Read {
    type Item = String;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<String>>> {
        lease(tx, "game IS NULL AND state = 'stored'", now, until)
    }

    /// Reads the copy and records which game it is. The bucket's file must be
    /// the whole replay it was stored as, its digest the copy's; anything
    /// else is tried again, and the games kept twice wait for it.
    async fn attempt(&self, state: &AppState, digest: &String) -> Outcome {
        let Some(secret) = &state.integrations.archive_secret_key else {
            return retry("no archive secret key".into());
        };
        let file = match self
            .0
            .get(secret, &key(digest), MAX_REPLAY, state.now())
            .await
        {
            Ok(file) => file,
            Err(error) => return retry(error),
        };
        let Some(replay) = Replay::parse(&file).filter(|replay| replay.digest_hex() == *digest)
        else {
            return retry(format!("{}: not the replay it was stored as", key(digest)));
        };
        let (digest, game) = (digest.clone(), replay.game_hex());
        match state
            .db
            .write(move |tx| identified(tx, &digest, &game))
            .await
        {
            Ok(()) => Outcome::Delivered,
            Err(error) => retry(format!("{error:?}")),
        }
    }

    fn record(&self, tx: &Transaction<'_>, leased: &Leased<String>, record: &Record) -> Result<()> {
        release(tx, leased, record)
    }
}

impl Queue for Remove {
    type Item = String;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<String>>> {
        lease(tx, "state = 'duplicate'", now, until)
    }

    async fn attempt(&self, state: &AppState, digest: &String) -> Outcome {
        let Some(secret) = &state.integrations.archive_secret_key else {
            return retry("no archive secret key".into());
        };
        match self.0.delete(secret, &key(digest), state.now()).await {
            Ok(()) => Outcome::Delivered,
            Err(error) => retry(error),
        }
    }

    fn record(&self, tx: &Transaction<'_>, leased: &Leased<String>, record: &Record) -> Result<()> {
        if settled(record).0 {
            tx.execute(
                "DELETE FROM archive_games WHERE digest = ?1 AND state = 'duplicate' AND attempts = ?2",
                params![leased.item, leased.attempt],
            )?;
            return Ok(());
        }
        release(tx, leased, record)
    }
}

/// A game's file in the bucket, as `delivery::archive` stored it.
fn key(digest: &str) -> String {
    format!("games/{digest}.emberreplay")
}

/// Leases up to `BATCH` games that are `due` (a condition on
/// `archive_games`) at `now`, in the columns the upload queue leased them
/// by, which it no longer uses for them. Its first attempt was the upload's,
/// so each lease starts the retry schedule afresh: these never give up.
fn lease(tx: &Transaction<'_>, due: &str, now: u64, until: u64) -> Result<Vec<Leased<String>>> {
    let rows = tx
        .prepare(&format!(
            "SELECT digest, attempts FROM archive_games
             WHERE {due} AND next_attempt_at <= ?1 ORDER BY next_attempt_at LIMIT ?2"
        ))?
        .query_map(params![now, BATCH], |row| {
            Ok(Leased {
                item: row.get(0)?,
                attempt: row.get::<_, u32>(1)? + 1,
                first_attempt_at: None,
            })
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for row in &rows {
        tx.execute(
            "UPDATE archive_games SET next_attempt_at = ?1, attempts = ?2 WHERE digest = ?3",
            params![until, row.attempt, row.item],
        )?;
    }
    Ok(rows)
}

/// Ends the lease of an attempt that leaves the game in the archive: it is
/// due again when the attempt says (at once after a read, which may have
/// left it a `duplicate` for `Remove`), with why it is not settled.
fn release(tx: &Transaction<'_>, leased: &Leased<String>, record: &Record) -> Result<()> {
    let (_, next, error) = settled(record);
    if let Some(error) = error {
        eprintln!("archive: {} not settled yet: {error}", leased.item);
    }
    tx.execute(
        "UPDATE archive_games SET next_attempt_at = ?1, last_error = ?2 WHERE digest = ?3 AND attempts = ?4",
        params![next, error, leased.item, leased.attempt],
    )?;
    Ok(())
}
