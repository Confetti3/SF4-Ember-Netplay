//! The archive's games kept before it knew them by `Replay::game`
//! (`archive::duplicates`): each copy is read back from the bucket to learn
//! which game it is, and each copy folding left over is deleted from the
//! bucket, then its row. A copy whose upload is still leased
//! (`next_attempt_at` ahead) is not deleted before that attempt is over, so
//! its file cannot land after the delete. The leases are kept in the columns
//! the upload queue leased the row by, which it no longer uses for it;
//! `attempts` goes on counting from the upload's, so a lease still fences
//! the one before it.
use std::sync::Arc;

use ember_protocol::replays::{MAX_REPLAY, Replay};
use rusqlite::{Transaction, params};

use super::{
    BATCH, Leased, Outcome, Queue, Record,
    archive::{key, retry, settled},
};
use crate::{AppState, archive::duplicates::identified, error::Result, s3::Bucket};

/// A copy waiting for the bucket, by its digest.
pub enum Copy {
    /// Stored, and not yet known by `game`.
    Unread(String),
    /// Folded into another copy of its game, its file still in the bucket.
    Duplicate(String),
}

impl Copy {
    fn digest(&self) -> &str {
        match self {
            Self::Unread(digest) | Self::Duplicate(digest) => digest,
        }
    }
}

pub struct Duplicates(pub Arc<Bucket>);

impl Queue for Duplicates {
    type Item = Copy;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<Copy>>> {
        let rows = tx
            .prepare(
                "SELECT digest, attempts, state = 'duplicate' FROM archive_games
                 WHERE ((game IS NULL AND state = 'stored') OR state = 'duplicate') AND next_attempt_at <= ?1
                 ORDER BY next_attempt_at LIMIT ?2",
            )?
            .query_map(params![now, BATCH], |row| {
                let digest = row.get(0)?;
                Ok(Leased {
                    item: if row.get(2)? {
                        Copy::Duplicate(digest)
                    } else {
                        Copy::Unread(digest)
                    },
                    attempt: row.get::<_, u32>(1)? + 1,
                    // The first attempt was the upload's: these never give up.
                    first_attempt_at: None,
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        for row in &rows {
            tx.execute(
                "UPDATE archive_games SET next_attempt_at = ?1, attempts = ?2 WHERE digest = ?3",
                params![until, row.attempt, row.item.digest()],
            )?;
        }
        Ok(rows)
    }

    /// An unread copy is known only by the whole replay its digest names;
    /// anything else the bucket gives back is tried again. A duplicate's file
    /// is deleted.
    async fn attempt(&self, state: &AppState, copy: &Copy) -> Outcome {
        let Some(secret) = &state.integrations.archive_secret_key else {
            return retry("no archive secret key".into());
        };
        let (digest, now) = (copy.digest().to_owned(), state.now());
        let done = match copy {
            Copy::Unread(_) => match self.0.get(secret, &key(&digest), MAX_REPLAY, now).await {
                Ok(file) => {
                    match Replay::parse(&file).filter(|replay| replay.digest_hex() == digest) {
                        Some(replay) => {
                            let game = replay.game_hex();
                            state
                                .db
                                .write(move |tx| identified(tx, &digest, &game))
                                .await
                                .map_err(|error| format!("{error:?}"))
                        }
                        None => Err(format!("{}: not the replay it was stored as", key(&digest))),
                    }
                }
                Err(error) => Err(error),
            },
            Copy::Duplicate(_) => self.0.delete(secret, &key(&digest), now).await,
        };
        match done {
            Ok(()) => Outcome::Delivered,
            Err(error) => retry(error),
        }
    }

    /// A deleted duplicate's row goes; any other attempt ends its lease, due
    /// again when the attempt says (at once after a read).
    fn record(&self, tx: &Transaction<'_>, leased: &Leased<Copy>, record: &Record) -> Result<()> {
        let (settled, next, error) = settled(record);
        let digest = leased.item.digest();
        if settled && matches!(leased.item, Copy::Duplicate(_)) {
            tx.execute(
                "DELETE FROM archive_games WHERE digest = ?1 AND state = 'duplicate' AND attempts = ?2",
                params![digest, leased.attempt],
            )?;
            return Ok(());
        }
        if let Some(error) = error {
            eprintln!("archive: {digest} not settled yet: {error}");
        }
        tx.execute(
            "UPDATE archive_games SET next_attempt_at = ?1, last_error = ?2 WHERE digest = ?3 AND attempts = ?4",
            params![next, error, digest, leased.attempt],
        )?;
        Ok(())
    }
}
