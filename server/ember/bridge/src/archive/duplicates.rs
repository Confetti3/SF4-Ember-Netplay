//! Games the archive kept before it knew them by `Replay::game`
//! (migration 054): both players' copies of one game, each kept under its
//! own digest with its own sender's side named. `delivery::duplicates` reads
//! each such copy back from the bucket and records which game it is
//! (`identified`). Once every copy is known, maintenance folds each game kept
//! more than once into one row (`fold_kept_before`), once; the copies left
//! over are marked `duplicate`, which the listing never shows, until the
//! bucket has deleted their files.
//!
//! Folding waits for the last copy because which copy is kept decides which
//! names the others may give it: a copy identified late may be the one that
//! came first. A set's copy is kept whenever there is one, and names come
//! from its roster, so a set's game is `fold`ed at once (`archive::admit`).
//!
//! `fold` and the removal of `duplicate`s stay: a set's copy folds its
//! players' online copies. Only the reading back of copies kept before
//! (`identified`, `fold_kept_before`, `archive_fold_owed`,
//! `delivery::duplicates::Copy::Unread`, `s3::Bucket::get`) can go once
//! production holds no row without a `game`.
use rusqlite::{OptionalExtension, Transaction, params};

use super::name_side;
use crate::error::Result;

/// Records that the stored copy `digest` is of `game`.
pub fn identified(tx: &Transaction<'_>, digest: &str, game: &str) -> Result<()> {
    tx.execute(
        "UPDATE archive_games SET game = ?2 WHERE digest = ?1 AND game IS NULL",
        params![digest, game],
    )?;
    Ok(())
}

/// Folds every game kept more than once, the first time no copy is left
/// that is not known by `game`; after that, nothing.
pub fn fold_kept_before(tx: &Transaction<'_>) -> Result<()> {
    let any = |query: &str| -> Result<bool> {
        Ok(tx.query_row(query, [], |_| Ok(())).optional()?.is_some())
    };
    if !any("SELECT 1 FROM archive_fold_owed")?
        || any("SELECT 1 FROM archive_games WHERE game IS NULL LIMIT 1")?
    {
        return Ok(());
    }
    let kept_twice = tx
        .prepare(
            "SELECT game FROM archive_games WHERE state != 'duplicate'
             GROUP BY game HAVING COUNT(*) > 1",
        )?
        .query_map([], |row| row.get::<_, String>(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for game in kept_twice {
        fold(tx, &game, None)?;
    }
    tx.execute("DELETE FROM archive_fold_owed", [])?;
    Ok(())
}

/// A kept copy of a game, as `fold` reads it.
struct Copy {
    digest: String,
    set: bool,
    sent_by: String,
    sent_side: Option<u8>,
    names: [Option<String>; 2],
    countries: [Option<String>; 2],
    named_by: [Option<String>; 2],
}

impl Copy {
    /// The account whose copy named side `side`: the first sender for its
    /// own side, otherwise whoever named it later.
    fn namer(&self, side: u8) -> Option<&str> {
        if self.sent_side == Some(side) {
            Some(&self.sent_by)
        } else {
            self.named_by[usize::from(side)].as_deref()
        }
    }
}

/// Makes `game` one row again: the set's copy, or else the copy that reached
/// the bridge first, is kept, and every other online copy, in the order they
/// came, gives it the sides it named by the rules a late copy names a side by
/// (`name_side`, which names no side of a set's game), then becomes a
/// `duplicate`. With `roster`, only the online copies its players sent are
/// folded: a set's copy takes in its own players' copies, never anyone
/// else's. A set's copy is never removed.
pub(super) fn fold(tx: &Transaction<'_>, game: &str, roster: Option<&[String]>) -> Result<()> {
    let copies = tx
        .prepare(
            "SELECT digest, match_id IS NOT NULL, sent_by, sent_side, name_a, name_b, country_a, country_b,
                    named_by_a, named_by_b
             FROM archive_games WHERE game = ?1 AND state != 'duplicate'
             ORDER BY match_id IS NULL, received_at, digest",
        )?
        .query_map([game], |row| {
            Ok(Copy {
                digest: row.get(0)?,
                set: row.get(1)?,
                sent_by: row.get(2)?,
                sent_side: row.get(3)?,
                names: [row.get(4)?, row.get(5)?],
                countries: [row.get(6)?, row.get(7)?],
                named_by: [row.get(8)?, row.get(9)?],
            })
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let Some((kept, others)) = copies.split_first() else {
        return Ok(());
    };
    let folded =
        |copy: &&Copy| !copy.set && roster.is_none_or(|players| players.contains(&copy.sent_by));
    for copy in others.iter().filter(folded) {
        for side in [0, 1] {
            if let Some(by) = copy.namer(side) {
                let at = usize::from(side);
                let (name, country) = (copy.names[at].as_deref(), copy.countries[at].as_deref());
                name_side(tx, &kept.digest, side, name, country, by)?;
            }
        }
        tx.execute(
            "UPDATE archive_games SET state = 'duplicate', replay = NULL WHERE digest = ?1",
            [&copy.digest],
        )?;
        crate::db::queued();
    }
    Ok(())
}
