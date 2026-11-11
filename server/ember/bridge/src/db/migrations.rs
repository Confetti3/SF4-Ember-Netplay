//! The schema: the SQL files under `migrations/`, applied in order, each once,
//! and recorded in `schema_migrations` by number, file name and a SHA-256 of
//! the file. A file that shipped keeps its name and its contents; a change is
//! a new file at the end.
//!
//! The bridge refuses to start on a database whose record of a migration
//! names another file or another checksum than its own, or that recorded a
//! number below its newest that it does not have. Either means the database
//! ran a migration from another line, and applying this bridge's on top of it
//! would skip one or break on one. A number above its newest is a newer
//! bridge's, which a bridge rolled back to leaves alone.
//!
//! Bridges before this recorded only the number. Their records take this
//! bridge's file for that number, once, when it first opens the database.
use rusqlite::{Connection, Transaction, TransactionBehavior, params};
use sha2::{Digest, Sha256};

use super::OpenError;
use crate::util::hex;

/// One file under `migrations/`; its name starts with its number.
#[derive(Clone, Copy)]
pub(super) struct Migration {
    pub version: i64,
    pub name: &'static str,
    pub sql: &'static str,
}

impl Migration {
    /// SHA-256 of the file without its carriage returns, so a Windows
    /// checkout and a Linux one record the same file alike.
    pub fn checksum(&self) -> String {
        let mut hash = Sha256::new();
        for part in self.sql.split('\r') {
            hash.update(part.as_bytes());
        }
        hex(&hash.finalize())
    }
}

/// The number a migration's file name starts with: three digits, then `_`.
/// A name without them fails the build.
const fn version_of(name: &str) -> i64 {
    let bytes = name.as_bytes();
    assert!(
        bytes.len() > 4 && bytes[3] == b'_',
        "a migration's name is its three-digit number, `_`, then what it does"
    );
    let mut version = 0;
    let mut at = 0;
    while at < 3 {
        assert!(
            bytes[at].is_ascii_digit(),
            "a migration's name starts with its number"
        );
        version = version * 10 + (bytes[at] - b'0') as i64;
        at += 1;
    }
    version
}

macro_rules! migrations {
    ($($name:literal),* $(,)?) => {
        &[$(Migration {
            version: version_of($name),
            name: $name,
            sql: include_str!(concat!("../../migrations/", $name)),
        }),*]
    };
}

pub(super) const MIGRATIONS: &[Migration] = migrations![
    "001_identity.sql",
    "002_matches.sql",
    "003_lobbies.sql",
    "004_records.sql",
    "005_tournaments.sql",
    "006_play.sql",
    "007_handoffs.sql",
    "008_discord.sql",
    "009_blumint.sql",
    "010_discord_consent.sql",
    "011_discord_moves.sql",
    "012_rooms.sql",
    "013_connection_settings.sql",
    "014_room_details.sql",
    "015_match_expiry.sql",
    "016_discord_confirm_owner.sql",
    "017_creator_region.sql",
    "018_room_own_delay.sql",
    "019_blumint_progress.sql",
    "020_match_starting_wins.sql",
    "021_blumint_status.sql",
    "022_match_claim_refusals.sql",
    "023_claim_liveness.sql",
    "024_blumint_keys.sql",
    "025_claim_ready.sql",
    "026_blumint_refused_keys_dropped.sql",
    "027_sign_in_sites.sql",
    "028_match_watch.sql",
    "029_skill_ratings.sql",
    "030_ranking_choices.sql",
    "031_delivery_order.sql",
    "032_organizer_grants.sql",
    "033_organizer_events.sql",
    "034_tournament_drafts.sql",
    "035_desk_sessions.sql",
    "036_desk_sets.sql",
    "037_match_replays.sql",
    "038_desk_marks.sql",
    "039_desk_calls.sql",
    "040_event_sign_ups.sql",
    "041_desk_session_seen.sql",
    "042_desk_handoffs.sql",
    "043_desk_devices.sql",
    "044_desk_device_checks.sql",
    "045_organizer_events_created.sql",
    "046_desk_dms.sql",
    "047_site_request_shares.sql",
    "048_organizer_invites.sql",
    "049_archive_games.sql",
    "050_archive_identity.sql",
    "051_archive_late_names.sql",
    "052_room_type.sql",
    "053_room_creator_in.sql",
    "054_archive_games_once.sql",
    "055_archive_listing.sql",
    "056_room_wired_only.sql",
    "057_fighter_levels.sql",
    "058_friends.sql",
    "059_account_points.sql",
];

/// Checks what the database recorded against `migrations`, then applies each
/// one it has not, in order, each in its own transaction.
pub(super) fn migrate(
    connection: &mut Connection,
    migrations: &[Migration],
) -> Result<(), OpenError> {
    let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
    record_table(&transaction)?;
    settle_unnamed_055(&transaction)?;
    check_records(&transaction, migrations)?;
    transaction.commit()?;
    for migration in migrations {
        let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
        if !recorded(&transaction, migration.version)? {
            transaction.execute_batch(migration.sql).map_err(|error| {
                OpenError::Migration(format!("migration {} failed: {error}", migration.name))
            })?;
            transaction.execute(
                "INSERT INTO schema_migrations (version, applied_at, name, checksum)
                 VALUES (?1, strftime('%s','now'), ?2, ?3)",
                params![migration.version, migration.name, migration.checksum()],
            )?;
        }
        transaction.commit()?;
    }
    Ok(())
}

/// The record, with a file's name and checksum, which are NULL on what a
/// bridge from before them recorded (also after a rollback to one).
fn record_table(tx: &Transaction<'_>) -> rusqlite::Result<()> {
    tx.execute_batch(
        "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)",
    )?;
    if !has_column(tx, "schema_migrations", "name")? {
        tx.execute_batch(
            "ALTER TABLE schema_migrations ADD COLUMN name TEXT;
             ALTER TABLE schema_migrations ADD COLUMN checksum TEXT;",
        )?;
    }
    Ok(())
}

/// Number 055 has meant two files. `055_room_wired_only.sql` was committed
/// under it and renumbered to 056 the same day, as 055 went to the archive
/// listing index the production bridge had already run. A bridge built in
/// between recorded the wired-only column as 55: left so, it would never run
/// the index, and 056 would fail on a column it already has.
///
/// An unnamed 55 is the index when its table is there; when only the column
/// is, and 56 is not recorded, it was the old file, whose record moves to 56
/// (the same file, byte for byte) so the index runs as 55. Anything else is
/// refused.
fn settle_unnamed_055(tx: &Transaction<'_>) -> Result<(), OpenError> {
    let unnamed: bool = tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM schema_migrations WHERE version = 55 AND name IS NULL)",
        [],
        |row| row.get(0),
    )?;
    if !unnamed || table_exists(tx, "archive_keys")? {
        return Ok(());
    }
    if has_column(tx, "rooms", "wired_only")? && !recorded(tx, 56)? {
        let wired_only = MIGRATIONS
            .iter()
            .find(|migration| migration.version == 56)
            .expect("056 is listed");
        tx.execute(
            "UPDATE schema_migrations SET version = 56, name = ?1, checksum = ?2 WHERE version = 55",
            params![wired_only.name, wired_only.checksum()],
        )?;
        return Ok(());
    }
    Err(OpenError::Migration(
        "migration 055 is recorded without a file name, and the database has neither the \
         archive listing index of 055_archive_listing.sql nor only the column of the old \
         055_room_wired_only.sql"
            .into(),
    ))
}

/// Gives each record without a name this bridge's file for its number, then
/// refuses a record of another file, of another checksum, or of a number
/// below the newest of `migrations` that they do not have.
fn check_records(tx: &Transaction<'_>, migrations: &[Migration]) -> Result<(), OpenError> {
    let newest = migrations.last().map_or(0, |migration| migration.version);
    let records: Vec<(i64, Option<String>, Option<String>)> = tx
        .prepare("SELECT version, name, checksum FROM schema_migrations ORDER BY version")?
        .query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))?
        .collect::<rusqlite::Result<_>>()?;
    for (version, name, checksum) in records {
        let Some(migration) = migrations
            .iter()
            .find(|migration| migration.version == version)
        else {
            if version < newest {
                return Err(OpenError::Migration(format!(
                    "the database recorded migration {version:03}, which this bridge does not have"
                )));
            }
            continue;
        };
        let expected = migration.checksum();
        let Some(name) = name else {
            tx.execute(
                "UPDATE schema_migrations SET name = ?2, checksum = ?3 WHERE version = ?1",
                params![version, migration.name, expected],
            )?;
            continue;
        };
        if name != migration.name || checksum.as_deref() != Some(expected.as_str()) {
            return Err(OpenError::Migration(format!(
                "migration {:03} is recorded as {name} with checksum {}, but this bridge's is {} with checksum {expected}",
                version,
                checksum.as_deref().unwrap_or("none"),
                migration.name,
            )));
        }
    }
    Ok(())
}

fn recorded(tx: &Transaction<'_>, version: i64) -> rusqlite::Result<bool> {
    tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM schema_migrations WHERE version = ?1)",
        [version],
        |row| row.get(0),
    )
}

fn table_exists(tx: &Transaction<'_>, table: &str) -> rusqlite::Result<bool> {
    tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1)",
        [table],
        |row| row.get(0),
    )
}

fn has_column(tx: &Transaction<'_>, table: &str, column: &str) -> rusqlite::Result<bool> {
    tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM pragma_table_info(?1) WHERE name = ?2)",
        [table, column],
        |row| row.get(0),
    )
}

#[cfg(test)]
#[path = "migrations_tests.rs"]
mod tests;
