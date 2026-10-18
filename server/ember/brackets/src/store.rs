//! The connector's SQLite file: each set's Ember match, the results the
//! bridge sent, the requests made with each site credential per month, and
//! the tournaments that have finished.
use std::{
    path::Path,
    sync::{Mutex, MutexGuard, PoisonError},
};

use rusqlite::{Connection, OptionalExtension, Row, params};

use crate::{
    sets::{Progress, Tracked},
    site::Need,
};

pub(crate) struct Store(Mutex<Connection>);

fn tracked_of(row: &Row<'_>) -> rusqlite::Result<Tracked> {
    let progress: String = row.get(4)?;
    Ok(Tracked {
        entries: [row.get(0)?, row.get(1)?],
        attempt: row.get(2)?,
        match_id: row.get(3)?,
        // Only `save` writes it, from a `Progress`.
        progress: Progress::parse(&progress).unwrap_or(Progress::Closed),
    })
}

impl Store {
    pub fn open(path: &Path) -> Result<Self, String> {
        let db = Connection::open(path)
            .map_err(|error| format!("cannot open {}: {error}", path.display()))?;
        db.execute_batch(
            "PRAGMA journal_mode = WAL; PRAGMA synchronous = FULL;
             CREATE TABLE IF NOT EXISTS sets (
                 bracket TEXT NOT NULL, set_id TEXT NOT NULL,
                 player1 TEXT NOT NULL, player2 TEXT NOT NULL,
                 attempt INTEGER NOT NULL, match_id TEXT, progress TEXT NOT NULL,
                 note TEXT,
                 PRIMARY KEY (bracket, set_id));
             CREATE TABLE IF NOT EXISTS results (
                 webhook_id TEXT PRIMARY KEY, bracket TEXT NOT NULL, body BLOB NOT NULL,
                 received_at INTEGER NOT NULL, done INTEGER NOT NULL);
             CREATE TABLE IF NOT EXISTS requests (
                 credential TEXT NOT NULL, month TEXT NOT NULL, count INTEGER NOT NULL,
                 PRIMARY KEY (credential, month));
             CREATE TABLE IF NOT EXISTS finished (
                 bracket TEXT PRIMARY KEY, finished_at INTEGER NOT NULL);",
        )
        .map_err(|error| format!("cannot prepare {}: {error}", path.display()))?;
        Ok(Self(Mutex::new(db)))
    }

    /// Every write is one statement or one transaction, so a panic elsewhere
    /// leaves nothing half done.
    fn db(&self) -> MutexGuard<'_, Connection> {
        self.0.lock().unwrap_or_else(PoisonError::into_inner)
    }

    pub fn tracked(&self, bracket: &str, set: &str) -> rusqlite::Result<Option<Tracked>> {
        self.db()
            .query_row(
                "SELECT player1, player2, attempt, match_id, progress FROM sets WHERE bracket = ?1 AND set_id = ?2",
                [bracket, set],
                tracked_of,
            )
            .optional()
    }

    /// The bracket's sets with a live Ember match, by set ID.
    pub fn live(&self, bracket: &str) -> rusqlite::Result<Vec<(String, Tracked)>> {
        let db = self.db();
        let mut statement = db.prepare(
            "SELECT player1, player2, attempt, match_id, progress, set_id FROM sets
             WHERE bracket = ?1 AND progress = 'live' ORDER BY set_id",
        )?;
        statement
            .query_map([bracket], |row| Ok((row.get(5)?, tracked_of(row)?)))?
            .collect()
    }

    /// Saves the set, keeping its note.
    pub fn save(&self, bracket: &str, set: &str, tracked: &Tracked) -> rusqlite::Result<()> {
        self.db().execute(
            "INSERT INTO sets (bracket, set_id, player1, player2, attempt, match_id, progress)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
             ON CONFLICT (bracket, set_id) DO UPDATE SET player1 = ?3, player2 = ?4,
                 attempt = ?5, match_id = ?6, progress = ?7",
            params![
                bracket,
                set,
                tracked.entries[0],
                tracked.entries[1],
                tracked.attempt,
                tracked.match_id,
                tracked.progress.as_str()
            ],
        )?;
        Ok(())
    }

    /// Records why a saved set waits, or that it no longer does. Returns
    /// whether that changed, so each reason is logged once.
    pub fn note(&self, bracket: &str, set: &str, note: Option<&str>) -> rusqlite::Result<bool> {
        let changed = self.db().execute(
            "UPDATE sets SET note = ?3 WHERE bracket = ?1 AND set_id = ?2 AND note IS NOT ?3",
            params![bracket, set, note],
        )?;
        Ok(changed == 1)
    }

    /// Records a result once. `done` when nothing is left to do with it.
    /// Returns whether it was new.
    pub fn receive(
        &self,
        webhook_id: &str,
        bracket: &str,
        body: &[u8],
        now: u64,
        done: bool,
    ) -> rusqlite::Result<bool> {
        let added = self.db().execute(
            "INSERT OR IGNORE INTO results (webhook_id, bracket, body, received_at, done)
             VALUES (?1, ?2, ?3, ?4, ?5)",
            params![webhook_id, bracket, body, now, done],
        )?;
        Ok(added == 1)
    }

    /// The bracket's results not dealt with yet, oldest first.
    pub fn pending(&self, bracket: &str) -> rusqlite::Result<Vec<(String, Vec<u8>)>> {
        let db = self.db();
        let mut statement = db.prepare(
            "SELECT webhook_id, body FROM results WHERE bracket = ?1 AND done = 0
             ORDER BY received_at, webhook_id",
        )?;
        statement
            .query_map([bracket], |row| Ok((row.get(0)?, row.get(1)?)))?
            .collect()
    }

    pub fn settled(&self, webhook_id: &str) -> rusqlite::Result<()> {
        self.db().execute(
            "UPDATE results SET done = 1 WHERE webhook_id = ?1",
            [webhook_id],
        )?;
        Ok(())
    }

    pub fn finished(&self, bracket: &str) -> rusqlite::Result<bool> {
        self.db()
            .query_row(
                "SELECT 1 FROM finished WHERE bracket = ?1",
                [bracket],
                |_| Ok(()),
            )
            .optional()
            .map(|found| found.is_some())
    }

    pub fn finish(&self, bracket: &str, now: u64) -> rusqlite::Result<()> {
        self.db().execute(
            "INSERT OR IGNORE INTO finished (bracket, finished_at) VALUES (?1, ?2)",
            params![bracket, now],
        )?;
        Ok(())
    }

    /// Counts one request with `credential` in `month`, unless it can wait
    /// and `limit` are spent. Returns the month's count, or `None` if the
    /// request is not to be made.
    pub fn count_request(
        &self,
        credential: &str,
        month: &str,
        limit: u32,
        need: Need,
    ) -> rusqlite::Result<Option<u32>> {
        let mut db = self.db();
        let tx = db.transaction()?;
        let used: u32 = tx
            .query_row(
                "SELECT count FROM requests WHERE credential = ?1 AND month = ?2",
                [credential, month],
                |row| row.get(0),
            )
            .optional()?
            .unwrap_or(0);
        if need == Need::Poll && used >= limit {
            return Ok(None);
        }
        tx.execute(
            "INSERT INTO requests (credential, month, count) VALUES (?1, ?2, 1)
             ON CONFLICT (credential, month) DO UPDATE SET count = count + 1",
            [credential, month],
        )?;
        tx.commit()?;
        Ok(Some(used + 1))
    }
}
