//! One SQLite connection in WAL mode with full fsync. Every write runs in
//! `BEGIN IMMEDIATE`, so writers are serialized and the uniqueness
//! constraints, not check-then-insert, decide races (spec 23.2).
use std::{
    path::Path,
    sync::{Arc, Mutex},
};

use rusqlite::{Connection, Transaction, TransactionBehavior};

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
];

#[derive(Clone)]
pub struct Db(Arc<Mutex<Connection>>);

impl Db {
    pub fn open(path: &Path) -> rusqlite::Result<Self> {
        let connection = Connection::open(path)?;
        connection.pragma_update(None, "journal_mode", "WAL")?;
        connection.pragma_update(None, "synchronous", "FULL")?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        connection.busy_timeout(std::time::Duration::from_secs(5))?;
        let mut db = connection;
        migrate(&mut db, MIGRATIONS)?;
        Ok(Self(Arc::new(Mutex::new(db))))
    }

    /// Runs `work` in an immediate (write) transaction on a blocking thread.
    pub async fn write<T, F>(&self, work: F) -> Result<T, ApiFailure>
    where
        T: Send + 'static,
        F: FnOnce(&Transaction<'_>) -> Result<T, ApiFailure> + Send + 'static,
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

    async fn run<T, F>(&self, behavior: TransactionBehavior, work: F) -> Result<T, ApiFailure>
    where
        T: Send + 'static,
        F: FnOnce(&Transaction<'_>) -> Result<T, ApiFailure> + Send + 'static,
    {
        let db = self.0.clone();
        tokio::task::spawn_blocking(move || {
            let mut connection = db.lock().map_err(|_| ApiFailure::unavailable())?;
            let transaction = connection.transaction_with_behavior(behavior)?;
            let value = work(&transaction)?;
            transaction.commit()?;
            Ok(value)
        })
        .await
        .map_err(|_| ApiFailure::unavailable())?
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
mod tests {
    use super::*;

    fn table_exists(connection: &Connection, name: &str) -> bool {
        connection
            .query_row(
                "SELECT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1)",
                [name],
                |row| row.get(0),
            )
            .unwrap()
    }

    #[test]
    fn a_database_from_before_discord_upgrades_to_the_current_schema() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection.pragma_update(None, "foreign_keys", "ON").unwrap();
        // Versions 1 to 7 as they shipped, the last with browser handoffs.
        migrate(&mut connection, &MIGRATIONS[..7]).unwrap();
        assert!(table_exists(&connection, "handoffs"));
        assert!(!table_exists(&connection, "discord_accounts"));

        migrate(&mut connection, MIGRATIONS).unwrap();
        assert!(!table_exists(&connection, "handoffs"));
        assert!(table_exists(&connection, "discord_accounts"));
        assert!(table_exists(&connection, "discord_sign_ins"));
        let links_sql: String = connection
            .query_row(
                "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'links'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert!(links_sql.contains("'discord'"), "{links_sql}");
        connection
            .prepare("SELECT delivery_attempts, delivery_next_at FROM matches")
            .unwrap();
        let versions: Vec<i64> = connection
            .prepare("SELECT version FROM schema_migrations ORDER BY version")
            .unwrap()
            .query_map([], |row| row.get(0))
            .unwrap()
            .collect::<rusqlite::Result<_>>()
            .unwrap();
        assert_eq!(versions, (1..=9).collect::<Vec<_>>());
    }
}
