//! The tombstone filter of retired process incarnations.
//!
//! A fixed-size Bloom filter lets expired route records be evicted without
//! ever making the exact retired incarnation admissible again. False
//! positives fail closed (an honest joiner is refused and starts over with a
//! fresh incarnation); there are no false negatives. The filter never clears
//! for the life of a room session, so its size sets how much churn a room
//! survives: a private room is short-lived and keeps the small filter, a
//! server-owned room is long-lived and gets one sized for many departures.
use crate::coordination::Ownership;

const HASHES: u64 = 3;
/// 8,192 bits: about 3% false positives after 1,000 departures.
const PRIVATE_WORDS: usize = 128;
/// 2^20 bits (128 KiB): about 2.2 in 100,000 after 10,000 departures and
/// 1.5% after 100,000.
const SERVER_OWNED_WORDS: usize = 1 << 14;

pub(super) struct RetiredFilter {
    room: [u8; 16],
    words: Vec<u64>,
}

impl RetiredFilter {
    pub fn new(room: [u8; 16], ownership: Ownership) -> Self {
        let words = if ownership.is_server_owned() {
            SERVER_OWNED_WORDS
        } else {
            PRIVATE_WORDS
        };
        Self {
            room,
            words: vec![0; words],
        }
    }

    fn index(&self, incarnation: u64, round: u64) -> usize {
        let mut value = incarnation ^ round.wrapping_mul(0x9e37_79b9_7f4a_7c15);
        value ^= u64::from_le_bytes(self.room[..8].try_into().unwrap()).rotate_left(17);
        value = value.wrapping_mul(0xbf58_476d_1ce4_e5b9).rotate_left(23);
        value ^= u64::from_le_bytes(self.room[8..].try_into().unwrap()).rotate_left(17);
        value = value.wrapping_mul(0xbf58_476d_1ce4_e5b9).rotate_left(23);
        (value as usize) % (self.words.len() * u64::BITS as usize)
    }

    pub fn contains(&self, incarnation: u64) -> bool {
        (0..HASHES).all(|round| {
            let bit = self.index(incarnation, round);
            self.words[bit / u64::BITS as usize] & (1u64 << (bit % u64::BITS as usize)) != 0
        })
    }

    pub fn insert(&mut self, incarnation: u64) {
        for round in 0..HASHES {
            let bit = self.index(incarnation, round);
            self.words[bit / u64::BITS as usize] |= 1u64 << (bit % u64::BITS as usize);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A fixed pseudo-random stream, so the rates below are reproducible.
    fn stream(seed: u64) -> impl Iterator<Item = u64> {
        let mut state = seed;
        std::iter::from_fn(move || {
            state = state.wrapping_add(0x9e37_79b9_7f4a_7c15);
            let mut z = state;
            z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
            Some((z ^ (z >> 31)).max(1))
        })
    }

    /// How many of `probes` unseen incarnations the filter claims to know
    /// after `departures` were inserted.
    fn false_positives(ownership: Ownership, departures: usize, probes: usize) -> usize {
        let mut filter = RetiredFilter::new([7; 16], ownership);
        let mut ids = stream(1);
        let retired: Vec<u64> = ids.by_ref().take(departures).collect();
        for id in &retired {
            filter.insert(*id);
        }
        assert!(
            retired.iter().all(|id| filter.contains(*id)),
            "no false negatives"
        );
        stream(2)
            .take(probes)
            .filter(|id| filter.contains(*id))
            .count()
    }

    #[test]
    fn a_private_room_filter_keeps_its_size_and_refuses_honest_joiners_under_churn() {
        assert_eq!(
            RetiredFilter::new([7; 16], Ownership::Private).words.len(),
            PRIVATE_WORDS
        );
        // About 3% of honest joiners are refused after 1,000 departures.
        let wrong = false_positives(Ownership::Private, 1_000, 100_000);
        assert!((2_000..4_000).contains(&wrong), "{wrong} of 100000");
    }

    #[test]
    fn a_server_owned_filter_survives_ten_thousand_departures() {
        let words = RetiredFilter::new([7; 16], Ownership::Host).words.len();
        assert_eq!(words * u64::BITS as usize, 1 << 20);
        assert_eq!(
            RetiredFilter::new([7; 16], Ownership::Member { host: 3 })
                .words
                .len(),
            words
        );
        // Expected 2.2e-5: about 4 of 200,000 unseen incarnations.
        let wrong = false_positives(Ownership::Host, 10_000, 200_000);
        assert!(wrong <= 20, "{wrong} of 200000");
    }
}
