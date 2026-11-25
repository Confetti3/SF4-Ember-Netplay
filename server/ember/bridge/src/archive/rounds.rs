//! Each archived game's final round score (`Replay::rounds`, migration 067),
//! which the listing shows so the page can say who won. `admit` keeps a new
//! game's with it. The games kept before migration 067 have none yet:
//! `delivery::rounds` reads each back from the bucket and records what its
//! replay says (`read`), a few games a pass, newest first, until no stored
//! game is left `unread`.
//!
//! A game is read once: one whose replay says nothing of its rounds, or that
//! the bucket no longer holds, is read as having none, so the backfill ends.
//! Only the backfill (`unread`, `read`, `delivery::rounds`) can go once
//! production holds no stored game without `rounds_read_at`.
use rusqlite::{Transaction, params};

use crate::error::Result;

/// The stored games whose rounds were never read, newest first, at most
/// `limit` of them, by digest. Read off migration 067's index of them alone,
/// never the index of every stored game by time, which would walk past the
/// games read already.
pub fn unread(tx: &Transaction<'_>, limit: i64) -> Result<Vec<String>> {
    Ok(tx
        .prepare(
            "SELECT digest FROM archive_games INDEXED BY archive_games_rounds_unread
             WHERE rounds_read_at IS NULL AND state = 'stored' ORDER BY saved_at DESC LIMIT ?1",
        )?
        .query_map([limit], |row| row.get(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?)
}

/// `rounds` as the game's row keeps them: `rounds_a` and `rounds_b`, both
/// or neither.
pub fn columns(rounds: Option<[u8; 2]>) -> [Option<u8>; 2] {
    rounds.map_or([None; 2], |rounds| rounds.map(Some))
}

/// The rounds a game's row keeps in `rounds_a` and `rounds_b`.
pub fn from_columns(a: Option<u8>, b: Option<u8>) -> Option<[u8; 2]> {
    a.zip(b).map(|(a, b)| [a, b])
}

/// Records that game `digest`'s replay, read at `now`, says `rounds` (none
/// when it says nothing of them, or could not be read), unless its rounds
/// were read already.
pub fn read(tx: &Transaction<'_>, digest: &str, rounds: Option<[u8; 2]>, now: u64) -> Result<()> {
    let [a, b] = columns(rounds);
    tx.execute(
        "UPDATE archive_games SET rounds_a = ?2, rounds_b = ?3, rounds_read_at = ?4
         WHERE digest = ?1 AND rounds_read_at IS NULL",
        params![digest, a, b, now],
    )?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Inserts a game as a bridge before migration 067 kept it: no rounds
    /// read, in `state`.
    fn kept_before(tx: &Transaction<'_>, digest: &str, saved_at: i64, state: &str) {
        tx.execute(
            "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, sent_by, priority,
                state, next_attempt_at, received_at)
             VALUES (?1, '00000000', ?2, 100, 1, 2, 'emb_player1', 1, ?3, 0, 0)",
            params![digest, saved_at, state],
        )
        .unwrap();
    }

    /// Only stored games are read back, newest first, a bounded number at a
    /// time, and each once.
    #[tokio::test]
    async fn the_backfill_reads_each_stored_game_once_newest_first() {
        let db = crate::Db::open(std::path::Path::new(":memory:")).unwrap();
        db.write(|tx| {
            for (digest, saved_at, state) in [
                ("old", 10, "stored"),
                ("new", 30, "stored"),
                ("middle", 20, "stored"),
                ("waiting", 40, "pending"),
                ("folded", 50, "duplicate"),
            ] {
                kept_before(tx, digest, saved_at, state);
            }
            assert_eq!(unread(tx, 2)?, ["new", "middle"]);
            read(tx, "new", Some([2, 1]), 100)?;
            read(tx, "middle", None, 100)?;
            assert_eq!(unread(tx, 2)?, ["old"]);
            // A game read already keeps what it was read as.
            read(tx, "new", Some([0, 2]), 200)?;
            let new: (Option<u8>, Option<u8>, Option<u64>) = tx.query_row(
                "SELECT rounds_a, rounds_b, rounds_read_at FROM archive_games WHERE digest = 'new'",
                [],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )?;
            assert_eq!(new, (Some(2), Some(1), Some(100)));
            read(tx, "old", None, 100)?;
            assert!(unread(tx, 2)?.is_empty());
            // Once stored, a game a bridge before 067 took is read too.
            tx.execute(
                "UPDATE archive_games SET state = 'stored' WHERE digest = 'waiting'",
                [],
            )?;
            assert_eq!(unread(tx, 2)?, ["waiting"]);
            Ok(())
        })
        .await
        .unwrap();
    }
}
