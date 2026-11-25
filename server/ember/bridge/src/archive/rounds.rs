//! Each archived game's final round score (`Replay::rounds`, migration 067),
//! which the listing shows so the page can say who won. `admit` keeps a new
//! game's with it. The games kept before migration 067 have none yet:
//! `delivery::rounds` reads each back from the bucket and records what its
//! replay says (`read`), a few games a pass, newest first, until no stored
//! game is left `unread`.
//!
//! A game is read once: one whose replay says nothing of its rounds, or that
//! the bucket no longer holds, is read as having none. One the bucket does
//! not answer for stays unread and is tried again on the backfill's next
//! sweep. Only the backfill (`unread`, `read`, `delivery::rounds`) can go
//! once production holds no stored game without `rounds_read_at`.
//!
//! The winner of a deciding round is in the slot record, which neither the
//! replay's CRC nor its `game` covers, so one copy of a game can name the
//! other winner than another. Two copies of a game that disagree leave it no
//! rounds (`dispute`): the archive cannot tell which is true.
use rusqlite::{Transaction, params};

use crate::error::Result;

/// A stored game whose rounds were never read, by its place in the
/// backfill's order: newest first, then by digest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Unread {
    pub saved_at: i64,
    pub digest: String,
}

/// The stored games whose rounds were never read, after `below` in the
/// backfill's order (from the newest when none), at most `limit` of them.
/// Read off migration 067's index of them alone, never the index of every
/// stored game by time, which would walk past the games read already.
pub fn unread(tx: &Transaction<'_>, below: Option<&Unread>, limit: i64) -> Result<Vec<Unread>> {
    let (saved_at, digest) = below
        .map(|game| (game.saved_at, game.digest.as_str()))
        .unzip();
    Ok(tx
        .prepare(
            "SELECT saved_at, digest FROM archive_games INDEXED BY archive_games_rounds_unread
             WHERE rounds_read_at IS NULL AND state = 'stored'
               AND (?1 IS NULL OR (saved_at, digest) < (?1, ?2))
             ORDER BY saved_at DESC, digest DESC LIMIT ?3",
        )?
        .query_map(params![saved_at, digest, limit], |row| {
            Ok(Unread {
                saved_at: row.get(0)?,
                digest: row.get(1)?,
            })
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?)
}

/// A copy of game `digest` that says `rounds` came: when it says some and
/// the game holds others, the game is left with none, read all the same, so
/// no later read gives it back either copy's. A copy that says nothing
/// contradicts nothing, and a game with none keeps none.
pub fn dispute(tx: &Transaction<'_>, digest: &str, rounds: Option<[u8; 2]>) -> Result<()> {
    let Some([a, b]) = rounds else {
        return Ok(());
    };
    tx.execute(
        "UPDATE archive_games SET rounds_a = NULL, rounds_b = NULL
         WHERE digest = ?1 AND rounds_a IS NOT NULL AND (rounds_a != ?2 OR rounds_b != ?3)",
        params![digest, a, b],
    )?;
    Ok(())
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

    fn digests(games: Vec<Unread>) -> Vec<String> {
        games.into_iter().map(|game| game.digest).collect()
    }

    type Row = (Option<u8>, Option<u8>, Option<u64>);

    fn row(tx: &Transaction<'_>, digest: &str) -> Row {
        tx.query_row(
            "SELECT rounds_a, rounds_b, rounds_read_at FROM archive_games WHERE digest = ?1",
            [digest],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
        )
        .unwrap()
    }

    /// Only stored games are read back, newest first (then by digest), a
    /// bounded number at a time from where the last left off, and each once.
    #[tokio::test]
    async fn the_backfill_reads_each_stored_game_once_newest_first() {
        let db = crate::Db::open(std::path::Path::new(":memory:")).unwrap();
        db.write(|tx| {
            for (digest, saved_at, state) in [
                ("old", 10, "stored"),
                ("new", 30, "stored"),
                ("middle", 20, "stored"),
                ("middle2", 20, "stored"),
                ("waiting", 40, "pending"),
                ("folded", 50, "duplicate"),
            ] {
                kept_before(tx, digest, saved_at, state);
            }
            let first = unread(tx, None, 2)?;
            assert_eq!(digests(first.clone()), ["new", "middle2"]);
            // Going on below the last game taken, read or not.
            assert_eq!(digests(unread(tx, first.last(), 2)?), ["middle", "old"]);
            read(tx, "new", Some([2, 1]), 100)?;
            read(tx, "middle", None, 100)?;
            assert_eq!(digests(unread(tx, None, 5)?), ["middle2", "old"]);
            // A game read already keeps what it was read as.
            read(tx, "new", Some([0, 2]), 200)?;
            assert_eq!(row(tx, "new"), (Some(2), Some(1), Some(100)));
            read(tx, "old", None, 100)?;
            read(tx, "middle2", None, 100)?;
            assert!(unread(tx, None, 2)?.is_empty());
            // Once stored, a game a bridge before 067 took is read too.
            tx.execute(
                "UPDATE archive_games SET state = 'stored' WHERE digest = 'waiting'",
                [],
            )?;
            assert_eq!(digests(unread(tx, None, 2)?), ["waiting"]);
            Ok(())
        })
        .await
        .unwrap();
    }

    /// A copy that agrees, or says nothing, leaves a game's rounds; one that
    /// disagrees leaves it none, still read, and later copies give none back.
    #[tokio::test]
    async fn copies_that_disagree_leave_a_game_no_rounds() {
        let db = crate::Db::open(std::path::Path::new(":memory:")).unwrap();
        db.write(|tx| {
            kept_before(tx, "game", 10, "stored");
            read(tx, "game", Some([2, 1]), 100)?;
            dispute(tx, "game", Some([2, 1]))?;
            dispute(tx, "game", None)?;
            assert_eq!(row(tx, "game"), (Some(2), Some(1), Some(100)));
            dispute(tx, "game", Some([1, 2]))?;
            assert_eq!(row(tx, "game"), (None, None, Some(100)));
            dispute(tx, "game", Some([2, 1]))?;
            read(tx, "game", Some([2, 1]), 200)?;
            assert_eq!(row(tx, "game"), (None, None, Some(100)));
            Ok(())
        })
        .await
        .unwrap();
    }
}
