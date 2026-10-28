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

/// A time something waits for, and a wait that doubles from `min` up to
/// `max` each time it grows, and is back at `min` after a reset. It times
/// the scheduled reads (from `poll_secs`, doubling while reads find
/// nothing new), the reads after a failed pass and each result whose report
/// waits on its own (a minute, doubling up to an hour).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Backoff {
    at: u64,
    /// The last wait, or `min` before any.
    wait: u64,
    min: u64,
    max: u64,
    /// Times it grew since it was made or reset.
    count: u32,
}

pub const RETRY_MIN_SECS: u64 = 60;
pub const RETRY_MAX_SECS: u64 = 60 * 60;

impl Backoff {
    pub fn new(min: u64, max: u64) -> Self {
        Self {
            at: 0,
            wait: min,
            min,
            max,
            count: 0,
        }
    }

    /// After a failure: a minute, doubling up to an hour, so a site that
    /// keeps failing costs a handful of requests an hour.
    pub fn retry() -> Self {
        Self::new(RETRY_MIN_SECS, RETRY_MAX_SECS)
    }

    pub fn ready(&self, now: u64) -> bool {
        now >= self.at
    }

    /// The time it is waiting for.
    pub fn at(&self) -> u64 {
        self.at
    }

    /// Waits from `now`: `min` the first time since a reset, then twice the
    /// last wait, at most `max`. Returns how many times it has grown since
    /// the reset, so the first of a run can be logged.
    pub fn grow(&mut self, now: u64) -> u32 {
        self.wait = if self.count == 0 {
            self.min
        } else {
            (self.wait * 2).min(self.max)
        };
        self.at = now + self.wait;
        self.count += 1;
        self.count
    }

    /// Back to `min` for the next wait, leaving the time waited for as it
    /// is. Returns how many times it had grown.
    pub fn reset(&mut self) -> u32 {
        self.wait = self.min;
        std::mem::take(&mut self.count)
    }

    /// Waits the last wait again from `now`, without growing it: a read
    /// that fails is not tried again sooner on the schedule.
    pub fn hold(&mut self, now: u64) {
        self.at = now + self.wait;
    }

    /// Waits `max` from `now`.
    pub fn longest(&mut self, now: u64) {
        self.wait = self.max;
        self.at = now + self.wait;
    }

    /// After a scheduled read that worked: `min` after a change, else the
    /// next, longer wait.
    pub fn read(&mut self, now: u64, changed: bool) {
        if changed {
            self.reset();
        }
        self.grow(now);
    }

    pub fn count(&self) -> u32 {
        self.count
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
        let mut schedule = Backoff::new(120, 600);
        assert!(schedule.ready(0));
        let mut waits = Vec::new();
        for changed in [true, false, false, false, false, true, false] {
            schedule.read(1_000, changed);
            waits.push(schedule.wait);
        }
        assert_eq!(waits, [120, 240, 480, 600, 600, 120, 240]);
        assert_eq!(schedule.at, 1_240);
        assert!(!schedule.ready(1_239));
        assert!(schedule.ready(1_240));
        // A failing read waits the current interval, without growing it.
        schedule.hold(2_000);
        assert_eq!((schedule.at, schedule.wait), (2_240, 240));
        schedule.longest(3_000);
        assert_eq!((schedule.at, schedule.wait), (3_600, 600));
        schedule.read(4_000, false);
        assert_eq!((schedule.at, schedule.wait), (4_600, 600));
    }

    #[test]
    fn failures_wait_longer_each_time_up_to_an_hour() {
        let mut retry = Backoff::retry();
        assert!(retry.ready(0));
        let mut waits = Vec::new();
        let mut now = 0;
        for failure in 1..=9 {
            assert_eq!(retry.grow(now), failure);
            let at = retry.at;
            assert!(!retry.ready(at - 1));
            assert!(retry.ready(at));
            waits.push(at - now);
            now = at;
        }
        assert_eq!(waits, [60, 120, 240, 480, 960, 1_920, 3_600, 3_600, 3_600]);
        assert_eq!(retry.count(), 9);
        assert_eq!(retry.reset(), 9);
        assert_eq!(retry.reset(), 0);
        // Ready as it was, and the next failure waits a minute again.
        assert!(retry.ready(now));
        retry.grow(now);
        assert_eq!(retry.at - now, 60);
    }
}
