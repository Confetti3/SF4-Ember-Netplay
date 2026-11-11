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

mod migrations;

/// Why `Db::open` could not open the database.
#[derive(Debug)]
pub enum OpenError {
    Sql(rusqlite::Error),
    /// The database's migrations are not this bridge's, or one failed; the
    /// text names the migration (`migrations`).
    Migration(String),
}

impl std::fmt::Display for OpenError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Sql(error) => std::fmt::Display::fmt(error, f),
            Self::Migration(text) => f.write_str(text),
        }
    }
}

impl std::error::Error for OpenError {}

impl From<rusqlite::Error> for OpenError {
    fn from(error: rusqlite::Error) -> Self {
        Self::Sql(error)
    }
}

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
    pub fn open(path: &Path) -> Result<Self, OpenError> {
        let connection = Connection::open(path)?;
        connection.pragma_update(None, "journal_mode", "WAL")?;
        connection.pragma_update(None, "synchronous", "FULL")?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        connection.busy_timeout(std::time::Duration::from_secs(5))?;
        let mut db = connection;
        migrations::migrate(&mut db, migrations::MIGRATIONS)?;
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_stored_name_not_known_fails_the_read_and_says_so() {
        let connection = Connection::open_in_memory().unwrap();
        let read = |text: &str| {
            connection.query_row("SELECT ?1", [text], |row| {
                known(row, 0, |text| (text == "queued").then_some(()))
            })
        };
        read("queued").unwrap();
        let error = read("a_later_state").unwrap_err();
        assert!(unknown(&error), "{error:?}");
        // Other failures are not mistaken for it.
        let error = connection
            .query_row("SELECT 1", [], |row| row.get::<_, String>(0))
            .unwrap_err();
        assert!(!unknown(&error));
    }
}
