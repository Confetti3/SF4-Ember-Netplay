//! How long a quorum read may take before it is given up on.
//!
//! The read behind `writable` is a chain of round trips, not one: a follower
//! asks the leader for its claim (one), the leader confirms leadership with a
//! heartbeat round to a majority of the voters (two), and a claim already in
//! flight for another caller is waited for first (three). Each over a relay
//! can lose a packet and pay a retransmission. A fixed two seconds fits those
//! on a short path and is far too little on a 300-480 ms one: reads then time
//! out at random, and the room flaps between "writable" and "recovering".
//! The deadline therefore follows the worst round trip measured to the voters.

use std::time::Duration;

/// The deadline on a short path, and the least there ever is.
pub const AUTHORITY_READ_FLOOR: Duration = Duration::from_secs(2);
/// More than this is a room that has lost its quorum, not a slow one.
pub const AUTHORITY_READ_CAP: Duration = Duration::from_secs(8);
/// The round trips one read can need, with room for a retransmission in each:
/// three exchanges at twice the measured time.
const AUTHORITY_READ_ROUND_TRIPS: u32 = 6;
const AUTHORITY_READ_MARGIN: Duration = Duration::from_secs(1);
/// Deadlines are whole steps, so a round trip that moves by a few
/// milliseconds does not move the deadline (or log a change).
const AUTHORITY_READ_STEP_MS: u64 = 500;

/// A quorum read's deadline and the measured round trip it came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ReadDeadline {
    pub deadline: Duration,
    /// The worst round trip to the voters; `None` until one is measured.
    pub rtt: Option<Duration>,
}

impl ReadDeadline {
    pub fn from_rtt(rtt: Option<Duration>) -> Self {
        let deadline = rtt.map_or(AUTHORITY_READ_FLOOR, |rtt| {
            let wanted = rtt
                .saturating_mul(AUTHORITY_READ_ROUND_TRIPS)
                .saturating_add(AUTHORITY_READ_MARGIN);
            let wanted_ms = u64::try_from(wanted.as_millis()).unwrap_or(u64::MAX);
            let stepped = wanted_ms.div_ceil(AUTHORITY_READ_STEP_MS) * AUTHORITY_READ_STEP_MS;
            Duration::from_millis(stepped).clamp(AUTHORITY_READ_FLOOR, AUTHORITY_READ_CAP)
        });
        Self { deadline, rtt }
    }

    pub fn deadline_ms(&self) -> u64 {
        u64::try_from(self.deadline.as_millis()).unwrap_or(u64::MAX)
    }

    /// Zero when no round trip is known.
    pub fn rtt_ms(&self) -> u64 {
        self.rtt
            .map_or(0, |rtt| u64::try_from(rtt.as_millis()).unwrap_or(u64::MAX))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ms(rtt: u64) -> u64 {
        ReadDeadline::from_rtt(Some(Duration::from_millis(rtt))).deadline_ms()
    }

    #[test]
    fn an_unmeasured_room_keeps_the_floor() {
        let unknown = ReadDeadline::from_rtt(None);
        assert_eq!(unknown.deadline, AUTHORITY_READ_FLOOR);
        assert_eq!(unknown.rtt_ms(), 0);
    }

    #[test]
    fn a_short_path_never_goes_below_the_floor() {
        assert_eq!(ms(1), 2000);
        assert_eq!(ms(40), 2000);
        assert_eq!(ms(160), 2000);
    }

    #[test]
    fn a_brazil_to_us_east_relay_gets_room_for_the_whole_chain() {
        // 6 x 300 + 1000 = 2800, 6 x 480 + 1000 = 3880: both above the old two seconds.
        assert_eq!(ms(300), 3000);
        assert_eq!(ms(480), 4000);
    }

    #[test]
    fn the_deadline_is_capped() {
        assert_eq!(ms(1200), 8000);
        assert_eq!(ms(60_000), 8000);
        assert_eq!(
            ReadDeadline::from_rtt(Some(Duration::MAX)).deadline,
            AUTHORITY_READ_CAP
        );
    }

    #[test]
    fn it_grows_with_the_round_trip_and_never_shrinks_below_it() {
        let mut last = 0;
        for rtt in (0..1500).step_by(7) {
            let deadline = ms(rtt);
            assert!(deadline >= last, "{rtt} ms went backwards");
            assert!(deadline >= 2000 && deadline <= 8000);
            last = deadline;
        }
    }

    #[test]
    fn jitter_inside_a_step_does_not_change_the_deadline() {
        assert_eq!(ms(310), ms(318));
        assert_eq!(ms(310), ms(330));
    }
}
