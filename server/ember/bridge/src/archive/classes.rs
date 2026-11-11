//! Each archived game's sides carry their players' classes on the public
//! rankings board (`class_a`, `class_b`, migration 055), so the listing can
//! pick and order games by class from its index. Classes move as players
//! play, settle, place again, list or leave, and as the bridge shows classes
//! or not: maintenance `refresh`es them every pass.
//!
//! A refresh compares the board with `archive_player_classes`, the classes
//! the archive's games were last given, and records each player whose class
//! moved as unsettled. It then gives unsettled players' games their new
//! class, at most `ROWS_PER_PASS` games a pass, so a change that touches many
//! games at once holds the database only briefly each pass. The listing
//! shows what the games carry, a pass or a few behind the board at most.
//!
//! While the bridge hides classes the games keep the classes they carry and
//! nothing refreshes them: the listing shows none and picks and orders by
//! none (`routes::archive::list`), so showing them again only moves the
//! classes that moved meanwhile.
use std::collections::HashMap;

use rusqlite::{Transaction, params};

use crate::{error::Result, skill::ranking};

/// Games given a new class in one pass at most.
pub(crate) const ROWS_PER_PASS: i64 = 1000;

/// Brings the classes the archive's games carry towards the board's, while
/// the bridge shows classes.
pub fn refresh(tx: &Transaction<'_>, shown: bool, developers: &[String], now: u64) -> Result<()> {
    if !shown {
        return Ok(());
    }
    refresh_with(tx, &ranking::public_classes(tx, developers, now)?)
}

/// One pass towards `board`.
pub(crate) fn refresh_with(tx: &Transaction<'_>, board: &HashMap<String, u8>) -> Result<()> {
    record(tx, board)?;
    apply(tx, ROWS_PER_PASS)
}

/// Records each player whose class on `board` is not the one the archive
/// last gave their games as unsettled, with their class now (none for a
/// player who left the board).
pub(crate) fn record(tx: &Transaction<'_>, board: &HashMap<String, u8>) -> Result<()> {
    let held: HashMap<String, Option<u8>> = tx
        .prepare("SELECT ember_id, band FROM archive_player_classes")?
        .query_map([], |row| Ok((row.get(0)?, row.get(1)?)))?
        .collect::<rusqlite::Result<_>>()?;
    let mut moved: Vec<(&str, Option<u8>)> = board
        .iter()
        .filter(|(player, band)| held.get(*player) != Some(&Some(**band)))
        .map(|(player, band)| (player.as_str(), Some(*band)))
        .collect();
    moved.extend(
        held.iter()
            .filter(|(player, band)| band.is_some() && !board.contains_key(*player))
            .map(|(player, _)| (player.as_str(), None)),
    );
    let mut upsert = tx.prepare(
        "INSERT INTO archive_player_classes (ember_id, band, settled) VALUES (?1, ?2, 0)
         ON CONFLICT (ember_id) DO UPDATE SET band = excluded.band, settled = 0",
    )?;
    for (player, band) in moved {
        upsert.execute(params![player, band])?;
    }
    Ok(())
}

/// Gives unsettled players' games their class, at most `rows` games, and
/// marks each player whose games all carry it settled (forgetting one who
/// left the board).
pub(crate) fn apply(tx: &Transaction<'_>, rows: i64) -> Result<()> {
    let unsettled: Vec<(String, Option<u8>)> = tx
        .prepare(
            "SELECT ember_id, band FROM archive_player_classes WHERE settled = 0 ORDER BY ember_id",
        )?
        .query_map([], |row| Ok((row.get(0)?, row.get(1)?)))?
        .collect::<rusqlite::Result<_>>()?;
    let mut left = rows;
    for (player, band) in unsettled {
        if left <= 0 {
            break;
        }
        let mut settled = true;
        for (player_column, class_column) in [("player_a", "class_a"), ("player_b", "class_b")] {
            let given = tx.execute(
                &format!(
                    "UPDATE archive_games SET {class_column} = ?2 WHERE digest IN
                        (SELECT digest FROM archive_games WHERE {player_column} = ?1
                           AND {class_column} IS NOT ?2 LIMIT ?3)"
                ),
                params![player, band, left],
            )? as i64;
            settled &= given < left;
            left -= given;
        }
        if settled {
            tx.execute(
                "DELETE FROM archive_player_classes WHERE ember_id = ?1 AND band IS NULL",
                [&player],
            )?;
            tx.execute(
                "UPDATE archive_player_classes SET settled = 1 WHERE ember_id = ?1",
                [&player],
            )?;
        }
    }
    Ok(())
}
