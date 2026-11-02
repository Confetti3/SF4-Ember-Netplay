//! Games the archive kept before it knew them by `Replay::game`
//! (migration 054): both players' copies of one game, each kept under its
//! own digest with its own sender's side named. `delivery::duplicates` reads
//! each such copy back from the bucket and `fold`s it into the game; the
//! copies it leaves over are marked `duplicate`, which the listing never
//! shows, until the bucket has deleted their files.
use rusqlite::{Transaction, params};

use super::name_side;
use crate::error::Result;

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

/// Records that the copy `digest` is of `game`, then makes that game one row
/// again: the set's copy, or else the copy that reached the bridge first, is
/// kept, and every other online copy gives it the sides it named, by the
/// rules a late copy names a side by (`name_side`), and becomes a
/// `duplicate`. A set's copy is never removed.
pub fn fold(tx: &Transaction<'_>, digest: &str, game: &str) -> Result<()> {
    tx.execute(
        "UPDATE archive_games SET game = ?2 WHERE digest = ?1 AND game IS NULL",
        params![digest, game],
    )?;
    let copies = tx
        .prepare(
            "SELECT digest, match_id IS NOT NULL, sent_by, sent_side, name_a, name_b, country_a, country_b,
                    named_by_a, named_by_b
             FROM archive_games WHERE game = ?1 AND state != 'duplicate'
             ORDER BY match_id IS NULL, state != 'stored', received_at, digest",
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
    for copy in others.iter().filter(|copy| !copy.set) {
        for side in [0, 1] {
            if let Some(by) = copy.namer(side) {
                name_side(tx, &kept.digest, side, &copy.names, &copy.countries, by)?;
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
