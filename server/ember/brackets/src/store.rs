//! The connector's SQLite file: each set's Ember match, the results received,
//! the requests made with each site credential per month, and the
//! tournaments that have finished.
use std::{
    path::Path,
    sync::{Mutex, MutexGuard, PoisonError},
};

use ember_protocol::{EmberId, matches::CreateMatch};
use rusqlite::{
    Connection, OptionalExtension, Row, params,
    types::{FromSql, FromSqlError, FromSqlResult, ToSql, ToSqlOutput, ValueRef},
};

use crate::sets::{Ended, ExternalId, Progress, Received, Tracked};

/// The schema this build writes, in `PRAGMA user_version`.
const SCHEMA_VERSION: u32 = 1;

/// The month a request counts in, in UTC, from Unix seconds.
const MONTH: &str = "strftime('%Y-%m', ?2, 'unixepoch')";

pub(crate) struct Store(Mutex<Connection>);

/// A stored Ember ID, checked on the way out.
struct StoredId(EmberId);

impl FromSql for StoredId {
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        let text = value.as_str()?;
        EmberId::parse(text)
            .map(Self)
            .map_err(|_| FromSqlError::InvalidType)
    }
}

impl FromSql for Progress {
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        Progress::parse(value.as_str()?).ok_or(FromSqlError::InvalidType)
    }
}

impl ToSql for Progress {
    fn to_sql(&self) -> rusqlite::Result<ToSqlOutput<'_>> {
        Ok(ToSqlOutput::from(self.as_str()))
    }
}

const TRACKED: &str = "player1, player2, ember1, ember2, attempt, match_id, progress";

fn tracked_of(row: &Row<'_>) -> rusqlite::Result<Tracked> {
    let ember: [Option<StoredId>; 2] = [row.get(2)?, row.get(3)?];
    Ok(Tracked {
        entries: [row.get(0)?, row.get(1)?],
        players: match ember {
            [Some(one), Some(two)] => Some([one.0, two.0]),
            _ => None,
        },
        attempt: row.get(4)?,
        match_id: row.get(5)?,
        progress: row.get(6)?,
    })
}

fn received_of(row: &Row<'_>) -> rusqlite::Result<(String, Received)> {
    let outcome: String = row.get(5)?;
    let ended = match outcome.as_str() {
        "completed" => Ended::Completed {
            games: [row.get(6)?, row.get(7)?],
            winner: row.get(8)?,
        },
        "restart" => Ended::Restart,
        "expired" => Ended::Expired,
        _ => {
            return Err(rusqlite::Error::FromSqlConversionFailure(
                5,
                rusqlite::types::Type::Text,
                "unknown outcome".into(),
            ));
        }
    };
    let [one, two]: [StoredId; 2] = [row.get(9)?, row.get(10)?];
    Ok((
        row.get(0)?,
        Received {
            id: ExternalId {
                bracket: row.get(1)?,
                set: row.get(2)?,
                attempt: row.get(3)?,
            },
            match_id: row.get(4)?,
            players: [one.0, two.0],
            ended,
        },
    ))
}

impl Store {
    pub fn open(path: &Path) -> Result<Self, String> {
        let db = Connection::open(path)
            .map_err(|error| format!("cannot open {}: {error}", path.display()))?;
        let prepare =
            |error: rusqlite::Error| format!("cannot prepare {}: {error}", path.display());
        let version: u32 = db
            .query_row("PRAGMA user_version", [], |row| row.get(0))
            .map_err(prepare)?;
        if version > SCHEMA_VERSION {
            return Err(format!(
                "{} was written by a newer ember-brackets (schema {version})",
                path.display()
            ));
        }
        // Only a test build before the first release wrote tables without
        // a version, in a shape this one cannot read.
        let unversioned: bool = db
            .query_row(
                "SELECT ?1 = 0 AND EXISTS (SELECT 1 FROM sqlite_master WHERE name = 'sets')",
                [version],
                |row| row.get(0),
            )
            .map_err(prepare)?;
        if unversioned {
            return Err(format!(
                "{} was written by a test build of ember-brackets; move it aside to start a new one",
                path.display()
            ));
        }
        db.execute_batch("PRAGMA journal_mode = WAL; PRAGMA synchronous = FULL;")
            .map_err(prepare)?;
        // The tables and their version are written together, so a first
        // start cut short leaves neither.
        db.execute_batch(&format!(
            "BEGIN IMMEDIATE;
             CREATE TABLE IF NOT EXISTS sets (
                 bracket TEXT NOT NULL, set_id TEXT NOT NULL,
                 player1 TEXT NOT NULL, player2 TEXT NOT NULL,
                 ember1 TEXT, ember2 TEXT,
                 attempt INTEGER NOT NULL, match_id TEXT, progress TEXT NOT NULL,
                 command TEXT, note TEXT,
                 PRIMARY KEY (bracket, set_id));
             CREATE TABLE IF NOT EXISTS results (
                 webhook_id TEXT PRIMARY KEY, bracket TEXT NOT NULL, set_id TEXT NOT NULL,
                 attempt INTEGER NOT NULL, match_id TEXT NOT NULL, outcome TEXT NOT NULL,
                 games1 INTEGER, games2 INTEGER, winner INTEGER,
                 ember1 TEXT NOT NULL, ember2 TEXT NOT NULL,
                 received_at INTEGER NOT NULL, done INTEGER NOT NULL);
             CREATE TABLE IF NOT EXISTS requests (
                 credential TEXT NOT NULL, month TEXT NOT NULL, count INTEGER NOT NULL,
                 PRIMARY KEY (credential, month));
             CREATE TABLE IF NOT EXISTS finished (
                 bracket TEXT PRIMARY KEY, finished_at INTEGER NOT NULL);
             PRAGMA user_version = {SCHEMA_VERSION};
             COMMIT;",
        ))
        .map_err(prepare)?;
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
                &format!("SELECT {TRACKED} FROM sets WHERE bracket = ?1 AND set_id = ?2"),
                [bracket, set],
                tracked_of,
            )
            .optional()
    }

    /// Whether any set of the bracket has been seen open.
    pub fn has_sets(&self, bracket: &str) -> rusqlite::Result<bool> {
        self.db()
            .query_row(
                "SELECT 1 FROM sets WHERE bracket = ?1 LIMIT 1",
                [bracket],
                |_| Ok(()),
            )
            .optional()
            .map(|found| found.is_some())
    }

    /// The bracket's sets with a live Ember match, by set ID.
    pub fn live(&self, bracket: &str) -> rusqlite::Result<Vec<(String, Tracked)>> {
        let db = self.db();
        let mut statement = db.prepare(&format!(
            "SELECT {TRACKED}, set_id FROM sets
             WHERE bracket = ?1 AND progress = 'live' ORDER BY set_id"
        ))?;
        statement
            .query_map([bracket], |row| Ok((row.get(7)?, tracked_of(row)?)))?
            .collect()
    }

    /// Saves the set, keeping its note, and forgets any creation saved for
    /// it: what is saved now replaces the attempt that creation was for.
    pub fn save(&self, bracket: &str, set: &str, tracked: &Tracked) -> rusqlite::Result<()> {
        self.save_with(bracket, set, tracked, None)
    }

    /// Saves the set with the creation about to be sent for it: a creation
    /// saved here is sent again as it was until the bridge has answered it.
    pub fn save_creating(
        &self,
        bracket: &str,
        set: &str,
        tracked: &Tracked,
        command: &CreateMatch,
    ) -> rusqlite::Result<()> {
        let text = serde_json::to_string(command)
            .map_err(|error| rusqlite::Error::ToSqlConversionFailure(Box::new(error)))?;
        self.save_with(bracket, set, tracked, Some(text))
    }

    fn save_with(
        &self,
        bracket: &str,
        set: &str,
        tracked: &Tracked,
        command: Option<String>,
    ) -> rusqlite::Result<()> {
        let [ember1, ember2] = tracked.players.as_ref().map_or([None, None], |[one, two]| {
            [Some(one.as_str()), Some(two.as_str())]
        });
        self.db().execute(
            "INSERT INTO sets (bracket, set_id, player1, player2, ember1, ember2, attempt,
                 match_id, progress, command)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)
             ON CONFLICT (bracket, set_id) DO UPDATE SET player1 = ?3, player2 = ?4,
                 ember1 = ?5, ember2 = ?6, attempt = ?7, match_id = ?8, progress = ?9,
                 command = ?10",
            params![
                bracket,
                set,
                tracked.entries[0],
                tracked.entries[1],
                ember1,
                ember2,
                tracked.attempt,
                tracked.match_id,
                tracked.progress,
                command
            ],
        )?;
        Ok(())
    }

    /// The creation saved for the set's current attempt and not answered yet.
    pub fn command(&self, bracket: &str, set: &str) -> rusqlite::Result<Option<CreateMatch>> {
        let text: Option<String> = self
            .db()
            .query_row(
                "SELECT command FROM sets WHERE bracket = ?1 AND set_id = ?2",
                [bracket, set],
                |row| row.get(0),
            )
            .optional()?
            .flatten();
        // One that no longer reads is an error, not a creation to make
        // afresh: the bridge may already have made its match.
        text.map(|text| {
            serde_json::from_str(&text).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(
                    0,
                    rusqlite::types::Type::Text,
                    Box::new(error),
                )
            })
        })
        .transpose()
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

    /// Records a result once, by its `webhook-id`. `done` when nothing is
    /// left to do with it. Returns whether it was new.
    pub fn receive(
        &self,
        webhook_id: &str,
        received: &Received,
        now: u64,
        done: bool,
    ) -> rusqlite::Result<bool> {
        let (outcome, games, winner) = match received.ended {
            Ended::Completed { games, winner } => ("completed", Some(games), Some(winner)),
            Ended::Restart => ("restart", None, None),
            Ended::Expired => ("expired", None, None),
        };
        let added = self.db().execute(
            "INSERT OR IGNORE INTO results (webhook_id, bracket, set_id, attempt, match_id,
                 outcome, games1, games2, winner, ember1, ember2, received_at, done)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)",
            params![
                webhook_id,
                received.id.bracket,
                received.id.set,
                received.id.attempt,
                received.match_id,
                outcome,
                games.map(|games| games[0]),
                games.map(|games| games[1]),
                winner,
                received.players[0].as_str(),
                received.players[1].as_str(),
                now,
                done
            ],
        )?;
        Ok(added == 1)
    }

    /// The bracket's results not dealt with yet, oldest first.
    pub fn pending(&self, bracket: &str) -> rusqlite::Result<Vec<(String, Received)>> {
        let db = self.db();
        let mut statement = db.prepare(
            "SELECT webhook_id, bracket, set_id, attempt, match_id, outcome, games1, games2,
                 winner, ember1, ember2
             FROM results WHERE bracket = ?1 AND done = 0 ORDER BY received_at, webhook_id",
        )?;
        statement.query_map([bracket], received_of)?.collect()
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

    /// Counts one request with `credential` in the month of `now`, unless
    /// it `can_wait` and `limit` are spent. Returns the month's count, or
    /// `None` if the request is not to be made.
    pub fn count_request(
        &self,
        credential: &str,
        now: u64,
        limit: u32,
        can_wait: bool,
    ) -> rusqlite::Result<Option<u32>> {
        let mut db = self.db();
        let tx = db.transaction()?;
        let used: u32 = tx
            .query_row(
                &format!("SELECT count FROM requests WHERE credential = ?1 AND month = {MONTH}"),
                params![credential, now],
                |row| row.get(0),
            )
            .optional()?
            .unwrap_or(0);
        if can_wait && used >= limit {
            return Ok(None);
        }
        tx.execute(
            &format!(
                "INSERT INTO requests (credential, month, count) VALUES (?1, {MONTH}, 1)
                 ON CONFLICT (credential, month) DO UPDATE SET count = count + 1"
            ),
            params![credential, now],
        )?;
        tx.commit()?;
        Ok(Some(used + 1))
    }

    /// Takes back one request counted in the month of `now`: the site
    /// refused it without serving it.
    pub fn uncount_request(&self, credential: &str, now: u64) -> rusqlite::Result<()> {
        self.db().execute(
            &format!(
                "UPDATE requests SET count = count - 1
                 WHERE credential = ?1 AND month = {MONTH} AND count > 0"
            ),
            params![credential, now],
        )?;
        Ok(())
    }

    /// The month's count, for tests and logs.
    #[cfg(test)]
    pub fn requests(&self, credential: &str, now: u64) -> rusqlite::Result<u32> {
        self.db()
            .query_row(
                &format!("SELECT count FROM requests WHERE credential = ?1 AND month = {MONTH}"),
                params![credential, now],
                |row| row.get(0),
            )
            .optional()
            .map(Option::unwrap_or_default)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn months_are_calendar_months_in_utc() {
        let dir = crate::test_dir("store");
        let store = Store::open(&dir.join("brackets.sqlite3")).unwrap();
        // 2000-02-29 is in February; 2027-01-01 starts a new year's count.
        for now in [
            951_782_399,
            951_782_400,
            951_868_800,
            1_798_761_599,
            1_798_761_600,
        ] {
            store.count_request("key", now, 10, false).unwrap();
        }
        let counts: Vec<u32> = [951_782_400, 951_868_800, 1_798_761_599, 1_798_761_600]
            .into_iter()
            .map(|now| store.requests("key", now).unwrap())
            .collect();
        assert_eq!(counts, [2, 1, 1, 1]);
        store.uncount_request("key", 951_782_400).unwrap();
        assert_eq!(store.requests("key", 951_782_400).unwrap(), 1);
        drop(store);
        // A file from a newer build, or from an unversioned test build, is
        // not opened.
        let db = Connection::open(dir.join("brackets.sqlite3")).unwrap();
        db.execute_batch("PRAGMA user_version = 99").unwrap();
        drop(db);
        assert!(Store::open(&dir.join("brackets.sqlite3")).is_err());
        let early = dir.join("early.sqlite3");
        let db = Connection::open(&early).unwrap();
        db.execute_batch("CREATE TABLE sets (bracket TEXT)")
            .unwrap();
        drop(db);
        assert!(Store::open(&early).err().unwrap().contains("test build"));
        // A new file is fine, and so is opening it again.
        let new = dir.join("new.sqlite3");
        drop(Store::open(&new).unwrap());
        drop(Store::open(&new).unwrap());
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
