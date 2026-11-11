//! Where a queue keeps its items, one row each, and the columns every queue
//! leases a row by and records an attempt's outcome in, under each table's
//! own names. A queue says what is due and how to send it; leasing, and the
//! fenced record of an outcome, are written here once.
use rusqlite::{ToSql, Transaction};

use super::{Record, Settled};
use crate::error::Result;

pub struct Table {
    pub name: &'static str,
    /// The columns that name one row, in the order `Queue::key` gives their
    /// values.
    pub key: &'static [&'static str],
    /// The number of the latest attempt, which a lease counts up and a record
    /// is fenced by.
    pub attempts: &'static str,
    /// When the row is due next, which a lease moves to its end.
    pub next: &'static str,
    pub outcomes: Outcomes,
}

/// How a table's rows say where each stands.
pub enum Outcomes {
    /// `pending` while due and while leased, then `delivered`, or `dead` when
    /// refused or out of retries, in `state`, with the attempt's error in
    /// `last_error` and the first attempt's time in `first_attempt_at`: the
    /// webhook outbox, desk calls and desk messages.
    Outbox,
    /// `queued` or `retrying` while due, `delivering` while leased, then
    /// `delivered` or `failed` (or `not_required`, which the queue decides),
    /// in `state`, with the first attempt's time in `first`; the queue audits
    /// each outcome: match results and BluMint's progress and status.
    Ledger {
        state: &'static str,
        first: &'static str,
    },
    /// The archive's queues: a lease leaves the row's state as it is, and the
    /// queue records each outcome its own way.
    Own,
}

impl Outcomes {
    /// The state `settled` leaves an outbox's or a ledger's row in.
    fn after(&self, settled: &Settled) -> &'static str {
        match (self, settled) {
            (_, Settled::Delivered) => "delivered",
            (Self::Outbox, Settled::Retry { .. }) => "pending",
            (Self::Outbox, _) => "dead",
            (_, Settled::Retry { .. }) => "retrying",
            _ => "failed",
        }
    }
}

impl Table {
    /// A table with the outbox's columns, as desk calls and messages copied
    /// them: rows named by `id`, `attempts`, `next_attempt_at` and the
    /// outbox's outcomes.
    pub const fn outbox(name: &'static str) -> Self {
        Self {
            name,
            key: &["id"],
            attempts: "attempts",
            next: "next_attempt_at",
            outcomes: Outcomes::Outbox,
        }
    }

    /// Leases the row `key` for the attempt numbered `attempt`: not due again
    /// before `until`, and its attempts counted up to that number, so the
    /// outcome of an earlier lease of it no longer applies.
    pub fn lease(
        &self,
        tx: &Transaction<'_>,
        key: &[&dyn ToSql],
        attempt: u32,
        until: u64,
    ) -> Result<()> {
        let marks = match &self.outcomes {
            Outcomes::Ledger { state, .. } => format!("{state} = 'delivering', "),
            Outcomes::Outbox | Outcomes::Own => String::new(),
        };
        let sql = format!(
            "UPDATE {} SET {marks}{} = ?1, {} = ?2 WHERE {}",
            self.name,
            self.next,
            self.attempts,
            self.key_from(3)
        );
        let mut values: Vec<&dyn ToSql> = vec![&until, &attempt];
        values.extend_from_slice(key);
        tx.execute(&sql, values.as_slice())?;
        Ok(())
    }

    /// Records `record` on the row `key` if it is still waiting under the
    /// lease numbered `attempt`. Answers the state it wrote, or None when a
    /// later lease owns the row or something since (a cancellation, a report
    /// queued again) moved it on.
    pub fn record(
        &self,
        tx: &Transaction<'_>,
        key: &[&dyn ToSql],
        attempt: u32,
        record: &Record,
    ) -> Result<Option<&'static str>> {
        let state = self.outcomes.after(&record.settled);
        let next = match &record.settled {
            Settled::Retry { at, .. } => *at,
            _ => record.at,
        };
        let error = record.settled.error();
        let mut values: Vec<&dyn ToSql> = vec![&state, &next, &record.first_attempt_at, &attempt];
        let sql = match &self.outcomes {
            Outcomes::Outbox => {
                values.push(&error);
                format!(
                    "UPDATE {} SET state = ?1, {} = ?2,
                        first_attempt_at = COALESCE(first_attempt_at, ?3), last_error = ?5
                     WHERE state = 'pending' AND {} = ?4 AND {}",
                    self.name,
                    self.next,
                    self.attempts,
                    self.key_from(6)
                )
            }
            Outcomes::Ledger {
                state: column,
                first,
            } => format!(
                "UPDATE {} SET {column} = ?1, {} = ?2, {first} = COALESCE({first}, ?3)
                 WHERE {column} = 'delivering' AND {} = ?4 AND {}",
                self.name,
                self.next,
                self.attempts,
                self.key_from(5)
            ),
            Outcomes::Own => unreachable!("{} records its own outcomes", self.name),
        };
        values.extend_from_slice(key);
        let changed = tx.execute(&sql, values.as_slice())?;
        Ok((changed == 1).then_some(state))
    }

    /// `column = ?n AND ...` for the key's columns, the first at `?from`.
    fn key_from(&self, from: usize) -> String {
        self.key
            .iter()
            .enumerate()
            .map(|(at, column)| format!("{column} = ?{}", from + at))
            .collect::<Vec<_>>()
            .join(" AND ")
    }
}

#[cfg(test)]
mod tests {
    use rusqlite::Connection;

    use super::*;
    use crate::delivery::{
        Queue, archive::Archive, calls::Calls, dms::Dms, duplicates::Duplicates, packs::Packs,
        progress::Progress, results::Results, status::Status, webhooks::Webhooks,
    };

    const OUTBOX: Table = Table::outbox("things");

    const LEDGER: Table = Table {
        name: "reports",
        key: &["match_id", "n"],
        attempts: "tries",
        next: "next_at",
        outcomes: Outcomes::Ledger {
            state: "status",
            first: "first_at",
        },
    };

    fn record(at: u64, settled: Settled) -> Record {
        Record {
            at,
            first_attempt_at: 5,
            settled,
        }
    }

    type Row = (String, u64, u32, Option<u64>);

    fn row(tx: &Transaction<'_>, sql: &str) -> Row {
        tx.query_row(sql, [], |row| {
            Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?))
        })
        .unwrap()
    }

    /// Every column a queue names is one its table has.
    #[tokio::test]
    async fn each_queue_names_the_columns_of_its_table() {
        let db = crate::Db::open(std::path::Path::new(":memory:")).unwrap();
        let tables = [
            Webhooks::TABLE,
            Calls::TABLE,
            Dms::TABLE,
            Results::TABLE,
            Progress::TABLE,
            Status::TABLE,
            Archive::TABLE,
            Duplicates::TABLE,
            Packs::TABLE,
        ];
        let unknown = db
            .read(move |tx| {
                Ok(tables
                    .iter()
                    .filter_map(|table| {
                        let mut columns: Vec<&str> = table.key.to_vec();
                        columns.extend([table.attempts, table.next]);
                        match &table.outcomes {
                            Outcomes::Outbox => {
                                columns.extend(["state", "first_attempt_at", "last_error"]);
                            }
                            Outcomes::Ledger { state, first } => columns.extend([*state, *first]),
                            Outcomes::Own => {}
                        }
                        let sql = format!("SELECT {} FROM {}", columns.join(", "), table.name);
                        tx.prepare(&sql)
                            .err()
                            .map(|error| format!("{sql}: {error}"))
                    })
                    .collect::<Vec<_>>())
            })
            .await
            .unwrap();
        assert!(unknown.is_empty(), "{unknown:?}");
    }

    #[test]
    fn an_outbox_row_takes_only_its_latest_leases_outcome() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection
            .execute_batch(
                "CREATE TABLE things (id INTEGER PRIMARY KEY, state TEXT NOT NULL,
                   next_attempt_at INTEGER NOT NULL, attempts INTEGER NOT NULL,
                   first_attempt_at INTEGER, last_error TEXT);
                 INSERT INTO things (id, state, next_attempt_at, attempts)
                   VALUES (1, 'pending', 0, 0), (2, 'pending', 0, 0);",
            )
            .unwrap();
        let tx = connection.transaction().unwrap();
        let one: &[&dyn ToSql] = &[&1i64];
        let read = |tx: &Transaction<'_>, id: i64| -> (Row, Option<String>) {
            let sql = format!(
                "SELECT state, next_attempt_at, attempts, first_attempt_at FROM things WHERE id = {id}"
            );
            let error = tx
                .query_row("SELECT last_error FROM things WHERE id = ?1", [id], |row| {
                    row.get(0)
                })
                .unwrap();
            (row(tx, &sql), error)
        };
        OUTBOX.lease(&tx, one, 1, 60).unwrap();
        assert_eq!(read(&tx, 1).0, ("pending".into(), 60, 1, None));
        // The first lease ran out and the row was leased again: the first
        // attempt's outcome no longer applies.
        OUTBOX.lease(&tx, one, 2, 120).unwrap();
        let delivered = record(70, Settled::Delivered);
        assert_eq!(OUTBOX.record(&tx, one, 1, &delivered).unwrap(), None);
        let retry = record(
            80,
            Settled::Retry {
                at: 90,
                error: "timeout".into(),
            },
        );
        assert_eq!(OUTBOX.record(&tx, one, 2, &retry).unwrap(), Some("pending"));
        assert_eq!(
            read(&tx, 1),
            (("pending".into(), 90, 2, Some(5)), Some("timeout".into()))
        );
        OUTBOX.lease(&tx, one, 3, 150).unwrap();
        let refused = record(100, Settled::Refused("status 404".into()));
        assert_eq!(OUTBOX.record(&tx, one, 3, &refused).unwrap(), Some("dead"));
        assert_eq!(
            read(&tx, 1),
            (("dead".into(), 100, 3, Some(5)), Some("status 404".into()))
        );
        // A dead row takes no later outcome, and the other row was never
        // touched.
        assert_eq!(OUTBOX.record(&tx, one, 3, &delivered).unwrap(), None);
        assert_eq!(read(&tx, 2), (("pending".into(), 0, 0, None), None));
    }

    #[test]
    fn a_ledger_row_is_delivering_while_leased_and_named_by_its_whole_key() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection
            .execute_batch(
                "CREATE TABLE reports (match_id TEXT NOT NULL, n INTEGER NOT NULL, status TEXT NOT NULL,
                   next_at INTEGER NOT NULL, tries INTEGER NOT NULL, first_at INTEGER,
                   PRIMARY KEY (match_id, n));
                 INSERT INTO reports (match_id, n, status, next_at, tries)
                   VALUES ('emt_1', 1, 'queued', 0, 0), ('emt_1', 2, 'queued', 0, 0);",
            )
            .unwrap();
        let tx = connection.transaction().unwrap();
        let first: &[&dyn ToSql] = &[&"emt_1", &1i64];
        let read = |tx: &Transaction<'_>, n: i64| {
            row(
                tx,
                &format!(
                    "SELECT status, next_at, tries, first_at FROM reports WHERE match_id = 'emt_1' AND n = {n}"
                ),
            )
        };
        LEDGER.lease(&tx, first, 1, 60).unwrap();
        assert_eq!(read(&tx, 1), ("delivering".into(), 60, 1, None));
        assert_eq!(read(&tx, 2), ("queued".into(), 0, 0, None));
        // Queued again while leased (a newer report): the lease's outcome no
        // longer applies, though its attempts still match.
        tx.execute("UPDATE reports SET status = 'queued' WHERE n = 1", [])
            .unwrap();
        let delivered = record(70, Settled::Delivered);
        assert_eq!(LEDGER.record(&tx, first, 1, &delivered).unwrap(), None);
        LEDGER.lease(&tx, first, 2, 120).unwrap();
        let expired = record(130, Settled::Expired("timeout".into()));
        assert_eq!(
            LEDGER.record(&tx, first, 2, &expired).unwrap(),
            Some("failed")
        );
        assert_eq!(read(&tx, 1), ("failed".into(), 130, 2, Some(5)));
        assert_eq!(read(&tx, 2), ("queued".into(), 0, 0, None));
    }
}
