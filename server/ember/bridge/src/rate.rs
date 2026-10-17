//! In-memory sliding-window limits (spec 25.2). Each policy owns its
//! admission budget; challenge traffic cannot fill signed-in routes' stores.
use std::{
    collections::{HashMap, VecDeque},
    sync::Mutex,
};

const MAX_BUCKETS_PER_POLICY: usize = 100_000;
const CLEANUP_SECS: u64 = 60;

pub struct Policies {
    pub challenges: Limiter,
    pub link_claims: Limiter,
    pub discord_starts: Limiter,
    pub room_tickets: Limiter,
}

impl Default for Policies {
    fn default() -> Self {
        Self {
            challenges: Limiter::new(30, 60, MAX_BUCKETS_PER_POLICY),
            link_claims: Limiter::new(10, 10 * 60, MAX_BUCKETS_PER_POLICY),
            discord_starts: Limiter::new(10, 3600, MAX_BUCKETS_PER_POLICY),
            room_tickets: Limiter::new(30, 10 * 60, MAX_BUCKETS_PER_POLICY),
        }
    }
}

#[derive(Default)]
struct State {
    buckets: HashMap<String, VecDeque<u64>>,
    last_cleanup: Option<u64>,
}

pub struct Limiter {
    limit: usize,
    window: u64,
    capacity: usize,
    state: Mutex<State>,
}

impl Limiter {
    fn new(limit: usize, window: u64, capacity: usize) -> Self {
        assert!(limit > 0 && window > 0 && capacity > 0);
        Self {
            limit,
            window,
            capacity,
            state: Mutex::default(),
        }
    }

    /// Records one use, or returns the seconds until another can be admitted.
    /// Keys are identities or accounts, not IP addresses (except room tickets,
    /// which deliberately apply the existing address-based admission policy).
    pub fn check(&self, key: &str, now: u64) -> Result<(), u64> {
        let mut state = self
            .state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let full = state.buckets.len() >= self.capacity && !state.buckets.contains_key(key);
        // At capacity, scan at most once per second, not for every refused key.
        let interval = if full { 1 } else { CLEANUP_SECS };
        if state
            .last_cleanup
            .is_none_or(|last| now < last || now - last >= interval)
        {
            state.buckets.retain(|_, uses| {
                uses.back()
                    .is_some_and(|last| now.saturating_sub(*last) < self.window)
            });
            state.last_cleanup = Some(now);
        }
        if state.buckets.len() >= self.capacity && !state.buckets.contains_key(key) {
            // Preserve active quotas instead of evicting and resetting them.
            return Err(1);
        }
        let uses = state.buckets.entry(key.to_owned()).or_default();
        while uses
            .front()
            .is_some_and(|first| now.saturating_sub(*first) >= self.window)
        {
            uses.pop_front();
        }
        if uses.len() >= self.limit {
            return Err(uses.front().map_or(self.window, |first| {
                if *first > now {
                    self.window.saturating_add(*first - now)
                } else {
                    self.window - (now - *first)
                }
            }));
        }
        // A backward wall-clock correction must not reorder the deque.
        uses.push_back(uses.back().copied().unwrap_or(now).max(now));
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn windows_slide() {
        let limiter = Limiter::new(3, 60, 10);
        for _ in 0..3 {
            assert_eq!(limiter.check("k", 100), Ok(()));
        }
        assert_eq!(limiter.check("k", 110), Err(50));
        assert_eq!(limiter.check("other", 110), Ok(()));
        assert_eq!(limiter.check("k", 160), Ok(()));
    }

    #[test]
    fn policies_have_independent_admission_and_cleanup() {
        let policies = Policies::default();
        for i in 0..MAX_BUCKETS_PER_POLICY {
            assert_eq!(policies.challenges.check(&format!("key-{i}"), 100), Ok(()));
        }
        assert_eq!(policies.challenges.check("overflow", 101), Err(1));
        for (limiter, limit, window) in [
            (&policies.link_claims, 10, 600),
            (&policies.discord_starts, 10, 3600),
            (&policies.room_tickets, 30, 600),
        ] {
            for _ in 0..limit {
                assert_eq!(limiter.check("signed-in", 100), Ok(()));
            }
            assert_eq!(limiter.check("signed-in", 101), Err(window - 1));
        }
        // Expiring challenge buckets cannot reset another policy's quota.
        assert_eq!(policies.challenges.check("new", 200), Ok(()));
        assert_eq!(policies.link_claims.check("signed-in", 200), Err(500));
        assert_eq!(policies.discord_starts.check("signed-in", 200), Err(3500));
        assert_eq!(policies.room_tickets.check("signed-in", 200), Err(500));
    }

    #[test]
    fn full_store_preserves_existing_quotas_and_reclaims_expired_buckets() {
        let limiter = Limiter::new(2, 60, 2);
        assert_eq!(limiter.check("a", 100), Ok(()));
        assert_eq!(limiter.check("b", 100), Ok(()));
        assert_eq!(limiter.check("c", 101), Err(1));
        assert_eq!(limiter.check("a", 101), Ok(()));
        assert_eq!(limiter.check("a", 101), Err(59));
        assert_eq!(limiter.state.lock().unwrap().buckets.len(), 2);
        assert_eq!(limiter.check("c", 160), Ok(()));
        assert_eq!(limiter.check("a", 160), Ok(()));
        assert_eq!(limiter.check("a", 160), Err(1));
    }

    #[test]
    fn cleanup_reclaims_expired_buckets_below_capacity() {
        let limiter = Limiter::new(1, 60, 10);
        assert_eq!(limiter.check("old", 100), Ok(()));
        assert_eq!(limiter.check("new", 160), Ok(()));
        assert_eq!(limiter.state.lock().unwrap().buckets.len(), 1);
    }

    #[test]
    fn backward_clock_step_reports_actual_retry_and_resets_cleanup_epoch() {
        let limiter = Limiter::new(1, 60, 1);
        assert_eq!(limiter.check("k", 100), Ok(()));
        assert_eq!(limiter.check("k", 90), Err(70));
        assert_eq!(limiter.state.lock().unwrap().last_cleanup, Some(90));
        assert_eq!(limiter.check("new", 91), Err(1));
        assert_eq!(limiter.state.lock().unwrap().last_cleanup, Some(91));
        assert_eq!(limiter.check("k", 150), Err(10));
        assert_eq!(limiter.check("new", 160), Ok(()));
    }

    #[test]
    fn arithmetic_saturates_and_recorded_uses_do_not_go_backwards() {
        let limiter = Limiter::new(2, 60, 10);
        assert_eq!(limiter.check("k", u64::MAX - 10), Ok(()));
        assert_eq!(limiter.check("k", u64::MAX - 20), Ok(()));
        assert_eq!(limiter.check("k", u64::MAX), Err(50));
        assert_eq!(limiter.check("k", 0), Err(u64::MAX));
    }
}
