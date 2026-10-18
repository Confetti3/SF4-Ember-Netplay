//! When a tournament's bracket is read: on a schedule only inside its
//! window, less often while nothing changes, and after a failure only once
//! a growing delay has passed. Pure, so the worker only asks.

/// The time a tournament may be read in. Either end may be open.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Window {
    /// The first second, included.
    pub from: Option<u64>,
    /// The first second after it.
    pub until: Option<u64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    Before,
    Open,
    After,
}

impl Window {
    pub fn phase(&self, now: u64) -> Phase {
        if self.from.is_some_and(|from| now < from) {
            Phase::Before
        } else if self.until.is_some_and(|until| now >= until) {
            Phase::After
        } else {
            Phase::Open
        }
    }
}

/// When the bracket is next read, and how long the wait grows while reads
/// find nothing new: from `min` it doubles after each read without a
/// change, up to `max`, and is back at `min` after any change.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Schedule {
    pub next_read: u64,
    pub interval: u64,
    min: u64,
    max: u64,
}

impl Schedule {
    pub fn new(min: u64, max: u64) -> Self {
        Self {
            next_read: 0,
            interval: min,
            min,
            max,
        }
    }

    pub fn due(&self, now: u64) -> bool {
        now >= self.next_read
    }

    /// Before a read: a read that fails is not tried again sooner than the
    /// current interval.
    pub fn reading(&mut self, now: u64) {
        self.next_read = now + self.interval;
    }

    /// After a read that worked.
    pub fn read(&mut self, now: u64, changed: bool) {
        self.interval = if changed {
            self.min
        } else {
            (self.interval * 2).min(self.max)
        };
        self.next_read = now + self.interval;
    }

    /// After a read that found the bracket played out: the longest wait.
    pub fn slow(&mut self, now: u64) {
        self.interval = self.max;
        self.next_read = now + self.interval;
    }
}

/// Reads after a failure: the first again after `RETRY_MIN_SECS`, each
/// later one after twice the wait, at most an hour, so a site that keeps
/// failing costs a handful of requests an hour.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Retry {
    at: u64,
    delay: u64,
    failures: u32,
}

pub const RETRY_MIN_SECS: u64 = 60;
pub const RETRY_MAX_SECS: u64 = 60 * 60;

impl Default for Retry {
    fn default() -> Self {
        Self {
            at: 0,
            delay: RETRY_MIN_SECS,
            failures: 0,
        }
    }
}

impl Retry {
    pub fn ready(&self, now: u64) -> bool {
        now >= self.at
    }

    /// Records a failure. Returns whether it starts a run of them, the one
    /// worth a log line.
    pub fn failed(&mut self, now: u64) -> bool {
        self.at = now + self.delay;
        self.delay = (self.delay * 2).min(RETRY_MAX_SECS);
        self.failures += 1;
        self.failures == 1
    }

    /// The failures recorded so far.
    pub fn failures(&self) -> u32 {
        self.failures
    }

    /// Records a success. Returns how many failures it ends.
    pub fn succeeded(&mut self) -> u32 {
        std::mem::take(self).failures
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_window_has_an_inside_and_two_outsides() {
        let window = Window {
            from: Some(100),
            until: Some(200),
        };
        let phases = [99, 100, 199, 200].map(|now| window.phase(now));
        assert_eq!(
            phases,
            [Phase::Before, Phase::Open, Phase::Open, Phase::After]
        );
        assert_eq!(Window::default().phase(0), Phase::Open);
        let open_ended = Window {
            from: Some(100),
            until: None,
        };
        assert_eq!(open_ended.phase(u64::MAX), Phase::Open);
    }

    #[test]
    fn quiet_reads_back_off_and_a_change_resets() {
        let mut schedule = Schedule::new(120, 600);
        assert!(schedule.due(0));
        let mut waits = Vec::new();
        for changed in [true, false, false, false, false, true, false] {
            schedule.read(1_000, changed);
            waits.push(schedule.interval);
        }
        assert_eq!(waits, [120, 240, 480, 600, 600, 120, 240]);
        assert_eq!(schedule.next_read, 1_240);
        assert!(!schedule.due(1_239));
        assert!(schedule.due(1_240));
        // A failing read waits the current interval, without growing it.
        schedule.reading(2_000);
        assert_eq!((schedule.next_read, schedule.interval), (2_240, 240));
        schedule.slow(3_000);
        assert_eq!((schedule.next_read, schedule.interval), (3_600, 600));
    }

    #[test]
    fn failures_wait_longer_each_time_up_to_an_hour() {
        let mut retry = Retry::default();
        assert!(retry.ready(0));
        let mut waits = Vec::new();
        let mut now = 0;
        for failure in 0..9 {
            assert_eq!(retry.failed(now), failure == 0);
            let at = retry.at;
            assert!(!retry.ready(at - 1));
            assert!(retry.ready(at));
            waits.push(at - now);
            now = at;
        }
        assert_eq!(waits, [60, 120, 240, 480, 960, 1_920, 3_600, 3_600, 3_600]);
        assert_eq!(retry.succeeded(), 9);
        assert_eq!(retry, Retry::default());
        assert_eq!(retry.succeeded(), 0);
    }
}
