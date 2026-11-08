//! One SQLite connection in WAL mode with full fsync. Every write runs in
//! `BEGIN IMMEDIATE`, so writers are serialized and the uniqueness
//! constraints, not check-then-insert, decide races (spec 23.2).
//!
//! A write that records an event or makes delivery work due says so
//! (`queued`), and once it commits the database counts it on `commits`, which
//! every event stream and delivery worker watches. A write that queued
//! nothing wakes nobody.
use std::{
    cell::Cell,
    path::Path,
    sync::{Arc, Mutex},
};

use rusqlite::{Connection, Transaction, TransactionBehavior};
use tokio::sync::watch;

use crate::error::ApiFailure;

/// Applied in order, each once, recorded by version. A version that shipped
/// keeps its number and its contents; a change is a new version at the end.
const MIGRATIONS: &[(i64, &str)] = &[
    (1, include_str!("../migrations/001_identity.sql")),
    (2, include_str!("../migrations/002_matches.sql")),
    (3, include_str!("../migrations/003_lobbies.sql")),
    (4, include_str!("../migrations/004_records.sql")),
    (5, include_str!("../migrations/005_tournaments.sql")),
    (6, include_str!("../migrations/006_play.sql")),
    (7, include_str!("../migrations/007_handoffs.sql")),
    (8, include_str!("../migrations/008_discord.sql")),
    (9, include_str!("../migrations/009_blumint.sql")),
    (10, include_str!("../migrations/010_discord_consent.sql")),
    (11, include_str!("../migrations/011_discord_moves.sql")),
    (12, include_str!("../migrations/012_rooms.sql")),
    (
        13,
        include_str!("../migrations/013_connection_settings.sql"),
    ),
    (14, include_str!("../migrations/014_room_details.sql")),
    (15, include_str!("../migrations/015_match_expiry.sql")),
    (
        16,
        include_str!("../migrations/016_discord_confirm_owner.sql"),
    ),
    (17, include_str!("../migrations/017_creator_region.sql")),
    (18, include_str!("../migrations/018_room_own_delay.sql")),
    (19, include_str!("../migrations/019_blumint_progress.sql")),
    (
        20,
        include_str!("../migrations/020_match_starting_wins.sql"),
    ),
    (21, include_str!("../migrations/021_blumint_status.sql")),
    (
        22,
        include_str!("../migrations/022_match_claim_refusals.sql"),
    ),
    (23, include_str!("../migrations/023_claim_liveness.sql")),
    (24, include_str!("../migrations/024_blumint_keys.sql")),
    (25, include_str!("../migrations/025_claim_ready.sql")),
    (
        26,
        include_str!("../migrations/026_blumint_refused_keys_dropped.sql"),
    ),
    (27, include_str!("../migrations/027_sign_in_sites.sql")),
    (28, include_str!("../migrations/028_match_watch.sql")),
    (29, include_str!("../migrations/029_skill_ratings.sql")),
    (30, include_str!("../migrations/030_ranking_choices.sql")),
    (31, include_str!("../migrations/031_delivery_order.sql")),
    (32, include_str!("../migrations/032_organizer_grants.sql")),
    (33, include_str!("../migrations/033_organizer_events.sql")),
    (34, include_str!("../migrations/034_tournament_drafts.sql")),
    (35, include_str!("../migrations/035_desk_sessions.sql")),
    (36, include_str!("../migrations/036_desk_sets.sql")),
    (37, include_str!("../migrations/037_match_replays.sql")),
    (38, include_str!("../migrations/038_desk_marks.sql")),
    (39, include_str!("../migrations/039_desk_calls.sql")),
    (40, include_str!("../migrations/040_event_sign_ups.sql")),
    (41, include_str!("../migrations/041_desk_session_seen.sql")),
    (42, include_str!("../migrations/042_desk_handoffs.sql")),
    (43, include_str!("../migrations/043_desk_devices.sql")),
    (44, include_str!("../migrations/044_desk_device_checks.sql")),
    (
        45,
        include_str!("../migrations/045_organizer_events_created.sql"),
    ),
    (46, include_str!("../migrations/046_desk_dms.sql")),
    (
        47,
        include_str!("../migrations/047_site_request_shares.sql"),
    ),
    (48, include_str!("../migrations/048_organizer_invites.sql")),
    (49, include_str!("../migrations/049_archive_games.sql")),
    (50, include_str!("../migrations/050_archive_identity.sql")),
    (51, include_str!("../migrations/051_archive_late_names.sql")),
    (52, include_str!("../migrations/052_room_type.sql")),
    (53, include_str!("../migrations/053_room_creator_in.sql")),
    (54, include_str!("../migrations/054_archive_games_once.sql")),
    // 55 is the archive listing index (feat/archive-server-class), already
    // applied on the production bridge; it lands with that branch.
    (56, include_str!("../migrations/056_room_wired_only.sql")),
    (57, include_str!("../migrations/057_room_game.sql")),
    (58, include_str!("../migrations/058_ranking_games.sql")),
    (59, include_str!("../migrations/059_match_games.sql")),
];

#[derive(Clone)]
pub struct Db(Arc<Shared>);

struct Shared {
    connection: Mutex<Connection>,
    /// Counts the commits that queued something (`queued`).
    commits: watch::Sender<u64>,
}

thread_local! {
    /// Set by `queued` while a transaction runs on this thread; `Db::run`
    /// clears it before the work and reads it after the commit. The work is
    /// synchronous and runs on the thread that commits it.
    static QUEUED: Cell<bool> = const { Cell::new(false) };
}

/// Marks the transaction running on this thread as one that recorded an
/// event or made delivery work due, so its commit wakes the event streams
/// and the delivery workers. Call it from the writers themselves
/// (`events::emit` and the delivery queues), not from routes.
pub(crate) fn queued() {
    QUEUED.with(|queued| queued.set(true));
}

impl Db {
    pub fn open(path: &Path) -> rusqlite::Result<Self> {
        let connection = Connection::open(path)?;
        connection.pragma_update(None, "journal_mode", "WAL")?;
        connection.pragma_update(None, "synchronous", "FULL")?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        connection.busy_timeout(std::time::Duration::from_secs(5))?;
        let mut db = connection;
        migrate(&mut db, MIGRATIONS)?;
        Ok(Self(Arc::new(Shared {
            connection: Mutex::new(db),
            commits: watch::channel(0).0,
        })))
    }

    /// A watch on the commits that queued something: it changes after each
    /// one commits.
    pub fn commits(&self) -> watch::Receiver<u64> {
        self.0.commits.subscribe()
    }

    /// Runs `work` in an immediate (write) transaction on a blocking thread.
    pub async fn write<T, F>(&self, work: F) -> Result<T, ApiFailure>
    where
        T: Send + 'static,
        F: FnOnce(&Transaction<'_>) -> Result<T, ApiFailure> + Send + 'static,
    {
        self.run(TransactionBehavior::Immediate, work).await
    }

    /// `write` for a route with an error type of its own: any `Err` from
    /// `work` rolls the write back, whatever the route answers it with.
    pub async fn write_with<T, E, F>(&self, work: F) -> Result<T, E>
    where
        T: Send + 'static,
        E: From<ApiFailure> + Send + 'static,
        F: FnOnce(&Transaction<'_>) -> Result<T, E> + Send + 'static,
    {
        self.run(TransactionBehavior::Immediate, work).await
    }

    /// Runs `work` in a read transaction, a consistent snapshot.
    pub async fn read<T, F>(&self, work: F) -> Result<T, ApiFailure>
    where
        T: Send + 'static,
        F: FnOnce(&Transaction<'_>) -> Result<T, ApiFailure> + Send + 'static,
    {
        self.run(TransactionBehavior::Deferred, work).await
    }

    async fn run<T, E, F>(&self, behavior: TransactionBehavior, work: F) -> Result<T, E>
    where
        T: Send + 'static,
        E: From<ApiFailure> + Send + 'static,
        F: FnOnce(&Transaction<'_>) -> Result<T, E> + Send + 'static,
    {
        let db = self.0.clone();
        tokio::task::spawn_blocking(move || {
            let mut connection = db
                .connection
                .lock()
                .map_err(|_| ApiFailure::unavailable())?;
            let transaction = connection
                .transaction_with_behavior(behavior)
                .map_err(ApiFailure::from)?;
            QUEUED.with(|queued| queued.set(false));
            let value = work(&transaction);
            // Taken whatever the outcome, so a rolled-back write leaves no mark.
            let queued = QUEUED.with(|queued| queued.replace(false));
            let value = value?;
            transaction.commit().map_err(ApiFailure::from)?;
            if queued {
                db.commits.send_modify(|count| *count += 1);
            }
            Ok(value)
        })
        .await
        .map_err(|_| ApiFailure::unavailable())?
    }
}

/// A stored name this bridge does not know, such as a state a newer bridge
/// wrote before a rollback. The value itself is not said: SQL errors are
/// logged, and they name columns, never stored values.
#[derive(Debug)]
pub struct Unknown;

impl std::fmt::Display for Unknown {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("a stored value this bridge does not know")
    }
}

impl std::error::Error for Unknown {}

/// Column `index` of `row` read as one of an enum's stored names. A name
/// `parse` does not know fails the read (`unknown` tells it apart) rather
/// than standing in for some other value.
pub fn known<T>(
    row: &rusqlite::Row<'_>,
    index: usize,
    parse: impl FnOnce(&str) -> Option<T>,
) -> rusqlite::Result<T> {
    let text: String = row.get(index)?;
    parse(&text).ok_or_else(|| {
        rusqlite::Error::FromSqlConversionFailure(
            index,
            rusqlite::types::Type::Text,
            Box::new(Unknown),
        )
    })
}

/// Whether `error` is a read that met a stored name this bridge does not
/// know (`known`, or a `FromSql` that answers `Unknown`).
pub fn unknown(error: &rusqlite::Error) -> bool {
    match error {
        rusqlite::Error::FromSqlConversionFailure(_, _, inner) => inner.is::<Unknown>(),
        _ => false,
    }
}

fn migrate(connection: &mut Connection, migrations: &[(i64, &str)]) -> rusqlite::Result<()> {
    connection.execute_batch(
        "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)",
    )?;
    for (version, sql) in migrations {
        let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
        let applied: bool = transaction.query_row(
            "SELECT EXISTS (SELECT 1 FROM schema_migrations WHERE version = ?1)",
            [version],
            |row| row.get(0),
        )?;
        if !applied {
            transaction.execute_batch(sql)?;
            transaction.execute(
                "INSERT INTO schema_migrations (version, applied_at) VALUES (?1, strftime('%s','now'))",
                [version],
            )?;
        }
        transaction.commit()?;
    }
    Ok(())
}

#[cfg(test)]
mod tests;
