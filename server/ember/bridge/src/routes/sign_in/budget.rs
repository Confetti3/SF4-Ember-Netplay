//! A site's allowance of requests, for a site that limits them
//! (`provider::Limits`, Challonge's): per calendar month and per day in UTC,
//! counted by day in the database so a restart does not reset them. A
//! sign-in's exchange takes its two requests (the token and `/me`) before it
//! calls the site, so neither limit is ever passed; one the site refuses
//! still counts both. Each refusal by a limit, the hourly start limit
//! included, is logged once per hour or day (`refused`), so an operator sees
//! a drain without the drain filling the log.
use rusqlite::{Transaction, params};

use super::provider::Limits;
use crate::{AppState, error::Result, linking::Site};

/// Requests a sign-in's exchange makes.
pub const PER_SIGN_IN: u64 = 2;

/// A limit that refused a sign-in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Cap {
    StartsPerHour,
    Day,
    Month,
}

impl Cap {
    /// The setting that sets it.
    fn setting(self) -> &'static str {
        match self {
            Self::StartsPerHour => "starts_per_hour",
            Self::Day => "daily_request_budget",
            Self::Month => "monthly_request_budget",
        }
    }

    /// How often a refusal by it is logged at most.
    fn logged_every(self) -> u64 {
        match self {
            Self::StartsPerHour => 3600,
            Self::Day | Self::Month => 24 * 3600,
        }
    }
}

/// Logs that `cap` refused a sign-in on `site`, unless it was logged within
/// the last hour (the start limit) or day (the budgets).
pub fn refused(state: &AppState, site: Site, cap: Cap) {
    let key = format!("{}-refused:{}", site.name(), cap.setting());
    if state
        .rate
        .check(&key, 1, cap.logged_every(), state.now())
        .is_ok()
    {
        eprintln!(
            "ember-bridge: {} sign-in refused: its {} is reached",
            site.name(),
            cap.setting()
        );
    }
}

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

/// The budget without room for a sign-in's exchange, if any.
fn full(today: u64, month: u64, limits: Limits) -> Option<Cap> {
    if month + PER_SIGN_IN > limits.monthly {
        Some(Cap::Month)
    } else if today + PER_SIGN_IN > limits.daily {
        Some(Cap::Day)
    } else {
        None
    }
}

/// Whether today's and this month's budgets still have a sign-in's
/// exchange in them. A refusal is logged.
pub async fn has_room(state: &AppState, site: Site, limits: Limits) -> Result<bool> {
    let now = state.now();
    let full = state
        .db
        .read(move |tx| {
            let (_, today, month) = spent(tx, site, now)?;
            Ok(full(today, month, limits))
        })
        .await?;
    if let Some(cap) = full {
        refused(state, site, cap);
    }
    Ok(full.is_none())
}

/// Takes a sign-in's exchange from today's and this month's budgets, or
/// says there is none left, logging the refusal. Logs a warning when the
/// month passes 80% of its budget.
pub async fn spend(state: &AppState, site: Site, limits: Limits) -> Result<bool> {
    let now = state.now();
    let spent = state
        .db
        .write(move |tx| {
            let (day, today, month) = spent(tx, site, now)?;
            if let Some(cap) = full(today, month, limits) {
                return Ok(Err(cap));
            }
            tx.execute(
                "INSERT INTO site_requests (site, day, spent) VALUES (?1, ?2, ?3)
                 ON CONFLICT (site, day) DO UPDATE SET spent = spent + excluded.spent",
                params![site.name(), day, PER_SIGN_IN],
            )?;
            Ok(Ok((month, month + PER_SIGN_IN)))
        })
        .await?;
    // Logged once the count is stored, so a rolled-back write does not warn.
    match spent {
        Ok((before, after)) => {
            if passes_warning(before, after, limits.monthly) {
                eprintln!(
                    "ember-bridge: {after} of this month's {} {} requests are spent; \
                     sign-in stops at the budget",
                    limits.monthly,
                    site.name()
                );
            }
            Ok(true)
        }
        Err(cap) => {
            refused(state, site, cap);
            Ok(false)
        }
    }
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
        assert_eq!(full(38, 100, limits), None);
        assert_eq!(full(39, 100, limits), Some(Cap::Day));
        assert_eq!(full(0, 399, limits), Some(Cap::Month));
        assert_eq!(full(39, 399, limits), Some(Cap::Month));
    }
}
