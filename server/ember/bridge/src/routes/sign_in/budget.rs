//! A site's allowance of requests, for a site that limits them
//! (`provider::Limits`, Challonge's): per calendar month and per day in UTC,
//! counted by day in the database so a restart does not reset them. A
//! sign-in's exchange takes its two requests (the token and `/me`) before it
//! calls the site, so neither limit is ever passed; one the site refuses
//! still counts both.
use rusqlite::{Transaction, params};

use super::provider::Limits;
use crate::{AppState, error::Result, linking::Site};

/// Requests a sign-in's exchange makes.
pub const PER_SIGN_IN: u64 = 2;

/// Today's key, and what was spent today and this month.
fn spent(tx: &Transaction<'_>, site: Site, now: u64) -> Result<(String, u64, u64)> {
    let day: String = tx.query_row(
        "SELECT strftime('%Y-%m-%d', ?1, 'unixepoch')",
        [now],
        |row| row.get(0),
    )?;
    let (today, month): (u64, u64) = tx.query_row(
        "SELECT COALESCE(SUM(CASE WHEN day = ?2 THEN spent END), 0), COALESCE(SUM(spent), 0)
         FROM site_requests WHERE site = ?1 AND day LIKE substr(?2, 1, 7) || '-%'",
        params![site.name(), day],
        |row| Ok((row.get(0)?, row.get(1)?)),
    )?;
    Ok((day, today, month))
}

/// Whether spending from `before` to `after` passes 80% of the month's
/// budget. Counts only grow within a month, so it happens once.
fn passes_warning(before: u64, after: u64, monthly: u64) -> bool {
    before * 5 < monthly * 4 && after * 5 >= monthly * 4
}

fn room(today: u64, month: u64, limits: Limits) -> bool {
    today + PER_SIGN_IN <= limits.daily && month + PER_SIGN_IN <= limits.monthly
}

/// Whether today's and this month's budgets still have a sign-in's
/// exchange in them.
pub async fn has_room(state: &AppState, site: Site, limits: Limits) -> Result<bool> {
    let now = state.now();
    state
        .db
        .read(move |tx| {
            let (_, today, month) = spent(tx, site, now)?;
            Ok(room(today, month, limits))
        })
        .await
}

/// Takes a sign-in's exchange from today's and this month's budgets, or
/// says there is none left. Logs a warning when the month passes 80% of its
/// budget.
pub async fn spend(state: &AppState, site: Site, limits: Limits) -> Result<bool> {
    let now = state.now();
    let spent = state
        .db
        .write(move |tx| {
            let (day, today, month) = spent(tx, site, now)?;
            if !room(today, month, limits) {
                return Ok(None);
            }
            tx.execute(
                "INSERT INTO site_requests (site, day, spent) VALUES (?1, ?2, ?3)
                 ON CONFLICT (site, day) DO UPDATE SET spent = spent + excluded.spent",
                params![site.name(), day, PER_SIGN_IN],
            )?;
            Ok(Some((month, month + PER_SIGN_IN)))
        })
        .await?;
    // Logged once the count is stored, so a rolled-back write does not warn.
    if let Some((before, after)) = spent
        && passes_warning(before, after, limits.monthly)
    {
        eprintln!(
            "ember-bridge: {after} of this month's {} {} requests are spent; \
             sign-in stops at the budget",
            limits.monthly,
            site.name()
        );
    }
    Ok(spent.is_some())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_warning_comes_once_a_month_at_eighty_percent() {
        let passes: Vec<u64> = (0..400)
            .step_by(PER_SIGN_IN as usize)
            .filter(|&before| passes_warning(before, before + PER_SIGN_IN, 400))
            .collect();
        assert_eq!(passes, [318]);
        let limits = Limits {
            monthly: 400,
            daily: 40,
            starts_per_hour: 30,
        };
        assert!(room(38, 100, limits));
        assert!(!room(39, 100, limits));
        assert!(!room(0, 399, limits));
    }
}
