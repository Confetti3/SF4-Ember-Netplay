//! The bridge's allowance of Challonge requests per calendar month in UTC
//! (`config::Challonge::monthly_request_budget`), kept in the database so a
//! restart does not reset it. Every request is taken from it before it is
//! sent: device codes, token polls and `/me` alike.
use rusqlite::{OptionalExtension, Transaction, params};

use crate::{config::Challonge, error::Result};

/// Takes up to `wanted` requests from this month's budget and says how many
/// it took. Logs a warning once a month, when 80% of it is spent.
pub fn take(tx: &Transaction<'_>, challonge: &Challonge, now: u64, wanted: u64) -> Result<u64> {
    let month: String =
        tx.query_row("SELECT strftime('%Y-%m', ?1, 'unixepoch')", [now], |row| {
            row.get(0)
        })?;
    let (spent, warned): (u64, bool) = tx
        .query_row(
            "SELECT spent, warned FROM challonge_requests WHERE month = ?1",
            [&month],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?
        .unwrap_or_default();
    let budget = challonge.monthly_request_budget;
    let taken = wanted.min(budget.saturating_sub(spent));
    if taken == 0 {
        return Ok(0);
    }
    let spent = spent + taken;
    let warn = !warned && spent * 5 >= budget * 4;
    if warn {
        eprintln!(
            "ember-bridge: {spent} of this month's {budget} Challonge requests are spent; \
             sign-ins stop at the budget"
        );
    }
    tx.execute(
        "INSERT INTO challonge_requests (month, spent, warned) VALUES (?1, ?2, ?3)
         ON CONFLICT (month) DO UPDATE SET spent = excluded.spent, warned = excluded.warned",
        params![month, spent, warned || warn],
    )?;
    Ok(taken)
}
