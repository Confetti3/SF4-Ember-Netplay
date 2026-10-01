//! In-memory sliding-window limits (spec 25.2). Keys are Ember IDs, intents
//! or accounts, not IP addresses, so shared networks stay usable.
use std::{
    collections::{HashMap, VecDeque},
    sync::Mutex,
};

#[derive(Default)]
pub struct Limiter(Mutex<HashMap<String, VecDeque<u64>>>);

impl Limiter {
    /// Records one use of `key` at `now`. `Err(retry_after)` when the
    /// window already holds `limit` uses.
    pub fn check(&self, key: &str, limit: usize, window: u64, now: u64) -> Result<(), u64> {
        let mut map = self
            .0
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if map.len() > 100_000 {
            map.retain(|_, uses| uses.back().is_some_and(|last| last + window > now));
        }
        let uses = map.entry(key.to_owned()).or_default();
        while uses.front().is_some_and(|first| first + window <= now) {
            uses.pop_front();
        }
        if uses.len() >= limit {
            return Err(uses.front().map_or(window, |first| first + window - now));
        }
        uses.push_back(now);
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
}
