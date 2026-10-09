//! In-memory sliding-window limits (spec 25.2). Keys are Ember IDs, intents
//! or accounts, not IP addresses, so shared networks stay usable.
use std::{
    collections::{HashMap, VecDeque},
    sync::Mutex,
};

const MAX_BUCKETS: usize = 100_000;
const CLEANUP_SECS: u64 = 60;

struct Bucket {
    window: u64,
    uses: VecDeque<u64>,
}

#[derive(Default)]
struct State {
    buckets: HashMap<String, Bucket>,
    last_cleanup: Option<u64>,
}

#[derive(Default)]
pub struct Limiter(Mutex<State>);

impl Limiter {
    /// Records one use of `key` at `now`. `Err(retry_after)` when the
    /// window already holds `limit` uses or no new bucket can be admitted.
    /// A key identifies one policy: callers must not reuse it across windows.
    pub fn check(&self, key: &str, limit: usize, window: u64, now: u64) -> Result<(), u64> {
        if limit == 0 || window == 0 {
            return Err(window.max(1));
        }
        let mut state = self
            .0
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let full = state.buckets.len() >= MAX_BUCKETS && !state.buckets.contains_key(key);
        // At capacity, scan at most once per second, not for every rejected key.
        let interval = if full { 1 } else { CLEANUP_SECS };
        if state
            .last_cleanup
            .is_none_or(|last| now.saturating_sub(last) >= interval)
        {
            state.buckets.retain(|_, bucket| {
                bucket
                    .uses
                    .back()
                    .is_some_and(|last| now.saturating_sub(*last) < bucket.window)
            });
            state.last_cleanup = Some(now);
        }
        if state.buckets.len() >= MAX_BUCKETS && !state.buckets.contains_key(key) {
            // Preserve active quotas: evicting them would reset rate limits.
            return Err(1);
        }
        let bucket = state
            .buckets
            .entry(key.to_owned())
            .or_insert_with(|| Bucket {
                window,
                uses: VecDeque::new(),
            });
        if bucket.window != window {
            return Err(bucket.window.max(window));
        }
        while bucket
            .uses
            .front()
            .is_some_and(|first| now.saturating_sub(*first) >= window)
        {
            bucket.uses.pop_front();
        }
        if bucket.uses.len() >= limit {
            return Err(bucket
                .uses
                .front()
                .map_or(window, |first| window - now.saturating_sub(*first)));
        }
        // A wall-clock correction must not make the deque run backwards.
        let recorded = bucket.uses.back().copied().unwrap_or(now).max(now);
        bucket.uses.push_back(recorded);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn windows_slide() {
        let limiter = Limiter::default();
        for _ in 0..3 {
            assert!(limiter.check("k", 3, 60, 100).is_ok());
        }
        assert_eq!(limiter.check("k", 3, 60, 110), Err(50));
        assert!(limiter.check("other", 3, 60, 110).is_ok());
        assert!(limiter.check("k", 3, 60, 160).is_ok());
    }

    #[test]
    fn short_window_cleanup_preserves_long_quotas() {
        let limiter = Limiter::default();
        assert_eq!(limiter.check("hourly", 1, 3600, 100), Ok(()));
        assert_eq!(limiter.check("ten-minute", 1, 600, 100), Ok(()));
        for i in 0..MAX_BUCKETS - 2 {
            assert_eq!(limiter.check(&format!("short-{i}"), 1, 60, 100), Ok(()));
        }
        assert_eq!(limiter.check("new-short", 1, 60, 200), Ok(()));
        assert_eq!(limiter.check("hourly", 1, 3600, 200), Err(3500));
        assert_eq!(limiter.check("ten-minute", 1, 600, 200), Err(500));
        assert_eq!(limiter.0.lock().unwrap().buckets.len(), 3);
    }

    #[test]
    fn full_store_rejects_new_keys_without_resetting_active_limits() {
        let limiter = Limiter::default();
        for i in 0..MAX_BUCKETS {
            assert_eq!(limiter.check(&format!("k-{i}"), 2, 60, 100), Ok(()));
        }
        for i in 0..20 {
            assert_eq!(limiter.check(&format!("overflow-{i}"), 1, 60, 101), Err(1));
        }
        assert_eq!(limiter.0.lock().unwrap().buckets.len(), MAX_BUCKETS);
        assert_eq!(limiter.check("k-0", 2, 60, 101), Ok(()));
        assert_eq!(limiter.check("k-0", 2, 60, 101), Err(59));
        assert_eq!(limiter.check("new", 1, 60, 160), Ok(()));
        assert_eq!(limiter.check("k-0", 2, 60, 160), Ok(()));
        assert_eq!(limiter.check("k-0", 2, 60, 160), Err(1));
    }

    #[test]
    fn long_window_cleanup_reclaims_short_buckets_below_capacity() {
        let limiter = Limiter::default();
        assert_eq!(limiter.check("short", 1, 60, 100), Ok(()));
        assert_eq!(limiter.check("hourly", 1, 3600, 160), Ok(()));
        assert_eq!(limiter.0.lock().unwrap().buckets.len(), 1);
    }

    #[test]
    fn time_arithmetic_does_not_overflow_or_reset_on_clock_rollback() {
        let limiter = Limiter::default();
        assert_eq!(limiter.check("k", 2, 60, u64::MAX - 10), Ok(()));
        assert_eq!(limiter.check("k", 2, 60, u64::MAX - 20), Ok(()));
        assert_eq!(limiter.check("k", 2, 60, u64::MAX), Err(50));
    }

    #[test]
    fn rejects_invalid_or_conflicting_policies_without_losing_quota() {
        let limiter = Limiter::default();
        assert_eq!(limiter.check("zero", 0, 60, 100), Err(60));
        assert_eq!(limiter.check("zero", 1, 0, 100), Err(1));
        assert!(limiter.0.lock().unwrap().buckets.is_empty());
        assert_eq!(limiter.check("k", 1, 3600, 100), Ok(()));
        assert_eq!(limiter.check("k", 1, 60, 101), Err(3600));
        assert_eq!(limiter.check("k", 1, 3600, 101), Err(3599));
    }
}
