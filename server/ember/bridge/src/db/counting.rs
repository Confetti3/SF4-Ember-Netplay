//! Counts the statements a piece of work runs, for the tests that hold a
//! read to a fixed number of queries whatever the number of players it reads
//! (`friends::state`, `cards::Reader` and the routes built on it).
use std::cell::Cell;

use rusqlite::{
    Transaction,
    trace::{TraceEvent, TraceEventCodes},
};

thread_local! {
    static STATEMENTS: Cell<usize> = const { Cell::new(0) };
}

fn count_statement(event: TraceEvent<'_>) {
    if matches!(event, TraceEvent::Stmt(..)) {
        STATEMENTS.with(|statements| statements.set(statements.get() + 1));
    }
}

/// What `work` returns, and the statements it ran on `tx`'s connection.
pub fn counted<T>(tx: &Transaction<'_>, work: impl FnOnce() -> T) -> (T, usize) {
    STATEMENTS.with(|statements| statements.set(0));
    tx.trace_v2(TraceEventCodes::SQLITE_TRACE_STMT, Some(count_statement));
    let value = work();
    tx.trace_v2(TraceEventCodes::SQLITE_TRACE_STMT, None);
    (value, STATEMENTS.with(Cell::get))
}
