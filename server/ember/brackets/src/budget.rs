//! Requests per site credential per calendar month. Challonge's free tier
//! allows 500 a month, so every request is counted before it is sent, the
//! operator is warned at 80%, and at 100% the connector stops reading
//! brackets but still reports the results it is sent.
use std::sync::Arc;

use crate::{
    Clock,
    site::{Need, SiteError},
    store::Store,
    time::month_key,
};

/// One site credential's monthly count.
pub(crate) struct Budget {
    store: Arc<Store>,
    /// The credential's file path: it names the credential in the count and
    /// in log lines, so the credential itself is never written anywhere.
    credential: String,
    limit: u32,
    clock: Clock,
}

impl Budget {
    pub fn new(store: Arc<Store>, credential: String, limit: u32, clock: Clock) -> Self {
        Self {
            store,
            credential,
            limit,
            clock,
        }
    }

    /// Counts one request about to be sent, or refuses one that can wait.
    pub fn spend(&self, need: Need) -> Result<(), SiteError> {
        let month = month_key(self.clock.now());
        let count = self
            .store
            .count_request(&self.credential, &month, self.limit, need)
            .map_err(|error| SiteError::Unavailable(format!("cannot count requests: {error}")))?
            .ok_or(SiteError::BudgetSpent)?;
        if count == warn_at(self.limit) && count < self.limit {
            crate::log!(
                "{}: {count} of {} requests used in {month}",
                self.credential,
                self.limit
            );
        }
        if count == self.limit {
            crate::log!(
                "{}: all {} requests of {month} used; brackets are not read again this month, results are still reported",
                self.credential,
                self.limit
            );
        }
        Ok(())
    }
}

/// 80% of `limit`, rounded up.
fn warn_at(limit: u32) -> u32 {
    (u64::from(limit) * 4).div_ceil(5) as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_warning_comes_at_eighty_percent() {
        assert_eq!(warn_at(500), 400);
        assert_eq!(warn_at(7), 6);
        assert_eq!(warn_at(1), 1);
    }

    #[test]
    fn polling_stops_at_the_budget_and_results_go_on() {
        let dir =
            std::env::temp_dir().join(format!("ember-brackets-budget-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let store = Arc::new(Store::open(&dir.join("brackets.sqlite3")).unwrap());
        let clock = Clock::default();
        let budget = Budget::new(store.clone(), "key-a".into(), 3, clock.clone());
        for _ in 0..3 {
            budget.spend(Need::Poll).unwrap();
        }
        assert!(matches!(
            budget.spend(Need::Poll),
            Err(SiteError::BudgetSpent)
        ));
        budget.spend(Need::Result).unwrap();
        assert_eq!(
            store
                .count_request("key-a", &month_key(clock.now()), 3, Need::Result)
                .unwrap(),
            Some(5)
        );
        // Another credential has its own count, and so does next month.
        Budget::new(store.clone(), "key-b".into(), 3, clock.clone())
            .spend(Need::Poll)
            .unwrap();
        clock.advance(32 * 86_400);
        budget.spend(Need::Poll).unwrap();
        drop((budget, store));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
