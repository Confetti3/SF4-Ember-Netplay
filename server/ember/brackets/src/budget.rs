//! Requests per site credential per calendar month. Challonge's free tier
//! allows 500 a month, so every request is counted before it is sent, the
//! operator is warned at 80%, and at 100% only requests that cannot wait are
//! made.
use std::sync::Arc;

use crate::{Clock, site::SiteError, store::Store};

/// Whether a request can wait for next month's budget.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Priority {
    CanWait,
    Always,
}

/// What a month's count has just reached.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Notice {
    /// 80% of the budget, rounded up.
    Warning,
    Spent,
}

fn notice(count: u32, limit: u32) -> Option<Notice> {
    let warn_at = (u64::from(limit) * 4).div_ceil(5);
    if count == limit {
        Some(Notice::Spent)
    } else if u64::from(count) == warn_at {
        Some(Notice::Warning)
    } else {
        None
    }
}

/// One site credential's monthly count.
pub(crate) struct Budget {
    store: Arc<Store>,
    /// A digest of the credential: the same key counts once wherever its
    /// file is, and the key itself is never stored.
    key: String,
    /// The credential's file, which names it in log lines.
    name: String,
    limit: u32,
    clock: Clock,
}

impl Budget {
    pub fn new(
        store: Arc<Store>,
        credential: &str,
        name: String,
        limit: u32,
        clock: Clock,
    ) -> Self {
        let digest = ember_protocol::json::digest(credential).unwrap_or_default();
        Self {
            store,
            key: format!("sha256:{digest}"),
            name,
            limit,
            clock,
        }
    }

    /// Counts one request about to be sent, or refuses one that can wait.
    pub fn spend(&self, priority: Priority) -> Result<(), SiteError> {
        let now = self.clock.now();
        let count = self
            .store
            .count_request(&self.key, now, self.limit, priority == Priority::CanWait)
            .map_err(|error| SiteError::Unavailable(format!("cannot count requests: {error}")))?
            .ok_or(SiteError::BudgetSpent)?;
        match notice(count, self.limit) {
            Some(Notice::Warning) => crate::log!(
                "{}: {count} of {} requests used this month",
                self.name,
                self.limit
            ),
            Some(Notice::Spent) => crate::log!(
                "{}: all {} requests of this month used; scheduled reads stop until next month, results are still reported",
                self.name,
                self.limit
            ),
            None => {}
        }
        Ok(())
    }

    /// Takes back a request the site refused without serving it.
    pub fn refund(&self) {
        let _ = self.store.uncount_request(&self.key, self.clock.now());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_warning_comes_at_eighty_percent_and_once() {
        let notices: Vec<Option<Notice>> = (398..=501).map(|count| notice(count, 500)).collect();
        assert_eq!(notices[..4], [None, None, Some(Notice::Warning), None]);
        assert_eq!(notices[102], Some(Notice::Spent));
        assert_eq!(notices[103], None);
        assert_eq!(notice(6, 7), Some(Notice::Warning));
        // With a budget of one the warning and the end are the same request.
        assert_eq!(notice(1, 1), Some(Notice::Spent));
    }

    #[test]
    fn requests_that_can_wait_stop_at_the_budget_and_others_go_on() {
        let dir = crate::test_dir("budget");
        let store = Arc::new(Store::open(&dir.join("brackets.sqlite3")).unwrap());
        let clock = Clock::frozen(1_792_350_000);
        let budget =
            |key: &str, name: &str| Budget::new(store.clone(), key, name.into(), 3, clock.clone());
        let first = budget("key-a", "a.key");
        for _ in 0..3 {
            first.spend(Priority::CanWait).unwrap();
        }
        assert!(matches!(
            first.spend(Priority::CanWait),
            Err(SiteError::BudgetSpent)
        ));
        first.spend(Priority::Always).unwrap();
        // The same key under another file name is the same count; a refused
        // request is taken back.
        let moved = budget("key-a", "moved.key");
        assert!(moved.spend(Priority::CanWait).is_err());
        moved.refund();
        moved.refund();
        moved.spend(Priority::CanWait).unwrap();
        // Another key has its own count, and so does next month.
        budget("key-b", "b.key").spend(Priority::CanWait).unwrap();
        clock.advance(32 * 86_400);
        first.spend(Priority::CanWait).unwrap();
        drop((first, moved, store));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
