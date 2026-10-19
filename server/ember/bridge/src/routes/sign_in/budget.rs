//! A site's allowance of requests, for a site whose definition limits them
//! (`provider::Limits`): per calendar month and per day in UTC,
//! counted by day in the database so a restart does not reset them. A
//! sign-in's exchange takes its two requests (the token and `/me`) before it
//! calls the site, so neither limit is ever passed; one the site refuses
//! still counts both. Each refusal by a limit, the hourly start limit
//! included, is logged once per hour or day (`refused`), so an operator sees
//! a drain without the drain filling the log.
use rusqlite::{Transaction, params};

use super::provider::Limits;
use crate::{AppState, error::Result};

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

    /// Seconds from `now` until a refusal by a budget may pass: the next
    /// UTC day for the day's. The month's is not worked out, and the hour's
    /// is the limiter's to say.
    pub fn retry_after(self, now: u64) -> Option<u64> {
        match self {
            Self::Day => Some(24 * 3600 - now % (24 * 3600)),
            Self::StartsPerHour | Self::Month => None,
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

/// Logs that `cap` refused a sign-in on site `site`, unless it was logged within
/// the last hour (the start limit) or day (the budgets).
pub fn refused(state: &AppState, site: &str, cap: Cap) {
    let key = format!("site-refused:{site}:{}", cap.setting());
    if state
        .rate
        .check(&key, 1, cap.logged_every(), state.now())
        .is_ok()
    {
        eprintln!(
            "ember-bridge: sign-in on {site} refused: its {} is reached",
            cap.setting()
        );
    }
}

/// Today's key, and what was spent today and this month.
fn spent(tx: &Transaction<'_>, site: &str, now: u64) -> Result<(String, u64, u64)> {
    let day: String = tx.query_row(
        "SELECT strftime('%Y-%m-%d', ?1, 'unixepoch')",
        [now],
        |row| row.get(0),
    )?;
    let (today, month): (u64, u64) = tx.query_row(
        "SELECT COALESCE(SUM(CASE WHEN day = ?2 THEN spent END), 0), COALESCE(SUM(spent), 0)
         FROM site_requests WHERE site = ?1 AND day LIKE substr(?2, 1, 7) || '-%'",
        params![site, day],
        |row| Ok((row.get(0)?, row.get(1)?)),
    )?;
    Ok((day, today, month))
}

/// Whether spending from `before` to `after` passes 80% of the month's
/// budget. Counts only grow within a month, so it happens once.
fn passes_warning(before: u64, after: u64, monthly: u64) -> bool {
    before * 5 < monthly * 4 && after * 5 >= monthly * 4
}

/// The budget, today's or this month's, without room for a sign-in's
/// exchange at `now`, if any, read in the caller's write.
pub fn full(tx: &Transaction<'_>, site: &str, now: u64, limits: Limits) -> Result<Option<Cap>> {
    let (_, today, month) = spent(tx, site, now)?;
    Ok(cap(today, month, limits))
}

/// The budget without room for a sign-in's exchange, if any.
fn cap(today: u64, month: u64, limits: Limits) -> Option<Cap> {
    if month + PER_SIGN_IN > limits.monthly_request_budget {
        Some(Cap::Month)
    } else if today + PER_SIGN_IN > limits.daily_request_budget {
        Some(Cap::Day)
    } else {
        None
    }
}

/// Takes a sign-in's exchange from today's and this month's budgets, or
/// says there is none left, logging the refusal. Logs a warning when the
/// month passes 80% of its budget.
pub async fn spend(state: &AppState, site: &str, limits: Limits) -> Result<bool> {
    let (now, id) = (state.now(), site.to_owned());
    let spent = state
        .db
        .write(move |tx| {
            let (day, today, month) = spent(tx, &id, now)?;
            if let Some(cap) = cap(today, month, limits) {
                return Ok(Err(cap));
            }
            tx.execute(
                "INSERT INTO site_requests (site, day, spent) VALUES (?1, ?2, ?3)
                 ON CONFLICT (site, day) DO UPDATE SET spent = spent + excluded.spent",
                params![id, day, PER_SIGN_IN],
            )?;
            Ok(Ok((month, month + PER_SIGN_IN)))
        })
        .await?;
    // Logged once the count is stored, so a rolled-back write does not warn.
    match spent {
        Ok((before, after)) => {
            if passes_warning(before, after, limits.monthly_request_budget) {
                eprintln!(
                    "ember-bridge: {after} of this month's {} requests to {site} are spent; \
                     sign-in stops at the budget",
                    limits.monthly_request_budget
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
            monthly_request_budget: 400,
            daily_request_budget: 40,
            starts_per_hour: 30,
        };
        assert_eq!(cap(38, 100, limits), None);
        assert_eq!(cap(39, 100, limits), Some(Cap::Day));
        assert_eq!(cap(0, 399, limits), Some(Cap::Month));
        assert_eq!(cap(39, 399, limits), Some(Cap::Month));
    }
}
