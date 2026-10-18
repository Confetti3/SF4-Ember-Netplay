//! A site's allowance of requests per calendar month in UTC, for a site that
//! limits them (Challonge's `monthly_request_budget`), kept in the database
//! so a restart does not reset it. A sign-in's exchange takes its two
//! requests (the token and `/me`) before it calls the site, so a month never
//! goes over; one the site refuses still counts both.
use rusqlite::{OptionalExtension, Transaction, params};

use crate::{AppState, error::Result, linking::Site};

/// Requests a sign-in's exchange makes.
pub const PER_SIGN_IN: u64 = 2;

/// This month's key and what was spent in it, with whether its warning was
/// logged.
fn month(tx: &Transaction<'_>, site: Site, now: u64) -> Result<(String, u64, bool)> {
    let month: String =
        tx.query_row("SELECT strftime('%Y-%m', ?1, 'unixepoch')", [now], |row| {
            row.get(0)
        })?;
    let (spent, warned) = tx
        .query_row(
            "SELECT spent, warned FROM site_requests WHERE site = ?1 AND month = ?2",
            params![site.name(), month],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?
        .unwrap_or_default();
    Ok((month, spent, warned))
}

/// Whether this month's budget still has a sign-in's exchange in it.
pub async fn has_room(state: &AppState, site: Site, budget: u64) -> Result<bool> {
    let now = state.now();
    state
        .db
        .read(move |tx| Ok(month(tx, site, now)?.1 + PER_SIGN_IN <= budget))
        .await
}

/// Takes a sign-in's exchange from this month's budget, or says there is
/// none left. Logs a warning once a month, when 80% of it is spent.
pub async fn spend(state: &AppState, site: Site, budget: u64) -> Result<bool> {
    let now = state.now();
    let spent = state
        .db
        .write(move |tx| {
            let (month, spent, warned) = month(tx, site, now)?;
            if spent + PER_SIGN_IN > budget {
                return Ok(None);
            }
            let spent = spent + PER_SIGN_IN;
            let warn = !warned && spent * 5 >= budget * 4;
            tx.execute(
                "INSERT INTO site_requests (site, month, spent, warned) VALUES (?1, ?2, ?3, ?4)
                 ON CONFLICT (site, month) DO UPDATE SET spent = excluded.spent, warned = excluded.warned",
                params![site.name(), month, spent, warned || warn],
            )?;
            Ok(Some(warn.then_some(spent)))
        })
        .await?;
    // Logged once the count is stored, so a rolled-back write does not warn.
    if let Some(Some(spent)) = spent {
        eprintln!(
            "ember-bridge: {spent} of this month's {budget} {} requests are spent; \
             sign-in stops at the budget",
            site.name()
        );
    }
    Ok(spent.is_some())
}
