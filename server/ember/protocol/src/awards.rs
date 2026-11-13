//! The counters behind the milestone awards (`src/netplay/Awards.hxx`), and
//! what a family id looks like. One type, `AwardCounts`, says them everywhere
//! they travel: in the account of a player's own points answer (`OwnAccount`
//! flattens it, so its members sit beside the points) and in the `awards`
//! member of another player's card. The game reads both into one
//! `awards::Counters`, and works out each family's level from it.
use serde::{Deserialize, Serialize};

use crate::{Error, Result, levels::is_zero, skill::SKILL_BANDS};

/// The longest award family id, in bytes. The bridge stores the ids a card
/// wears and shows, and `is_family_id` is the one rule for their shape; the
/// game's `profiles::FamilyIdShaped` states the same rule.
pub const MAX_FAMILY_ID: usize = 32;

/// Whether `text` is shaped like an award family's id: 1 to `MAX_FAMILY_ID`
/// bytes of `a-z`, `0-9` and `-`. The bridge checks the shape only, so a
/// family a later catalog adds needs no bridge change.
pub fn is_family_id(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= MAX_FAMILY_ID
        && text
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
}

/// The largest rival level's games against one opponent that a card shows:
/// the milestones of the `rival` family in the game's catalog, in order.
/// `awards_test` in the protocol's tests reads the catalog and fails when the
/// two differ.
pub const RIVAL_MARKS: [u64; 4] = [10, 50, 100, 250];
/// The milestones of the `opponents` family, in order.
pub const OPPONENT_MARKS: [u64; 4] = [10, 50, 200, 500];

/// The largest of `marks` (ascending) that `count` has reached; 0 for none.
pub fn mark(marks: &[u64], count: u64) -> u64 {
    marks
        .iter()
        .rev()
        .copied()
        .find(|mark| count >= *mark)
        .unwrap_or(0)
}

/// The counters an award level is reached from. In a player's own answer they
/// are exact. On another player's card the games, wins, best run and class
/// peak are exact too (the card's record cells show the first three), and
/// `rival` and `opponents` are the largest milestone reached (`shown`), so a
/// card says no more of who a player has played than the award does. Every
/// member but `games` and `wins` is absent while 0, and a bridge that does not
/// send one reads as 0 and the award as locked.
///
/// Wire names: `games`, `wins`, `best_streak`, `peak_class`, `rival`,
/// `opponents`, in the account of `OwnLevels` and in a card's `awards`. The
/// game keeps them in `awards::Counters` under the same names, in camel case.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AwardCounts {
    /// Counted games.
    #[serde(default)]
    pub games: u64,
    /// Counted wins, at most `games`.
    #[serde(default)]
    pub wins: u64,
    /// The most counted games the player won in a row, at most `games`.
    #[serde(default, skip_serializing_if = "is_zero")]
    pub best_streak: u32,
    /// The highest class ever shown, 1 to `SKILL_BANDS`: a peak, so it never
    /// goes down when the class does. 0 while there is none, and on a card of
    /// a player who does not take part in ranking, who shows no class.
    #[serde(default, skip_serializing_if = "is_zero")]
    pub peak_class: u8,
    /// The most counted games against one opponent.
    #[serde(default, skip_serializing_if = "is_zero")]
    pub rival: u32,
    /// How many different opponents the player has played.
    #[serde(default, skip_serializing_if = "is_zero")]
    pub opponents: u32,
}

impl AwardCounts {
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }

    /// These counts as a card shows them to anyone: `rival` and `opponents`
    /// as the largest milestone reached, and no class for a player who does
    /// not take part in `ranked`.
    pub fn shown(self, ranked: bool) -> Self {
        Self {
            rival: mark(&RIVAL_MARKS, u64::from(self.rival)) as u32,
            opponents: mark(&OPPONENT_MARKS, u64::from(self.opponents)) as u32,
            peak_class: if ranked { self.peak_class } else { 0 },
            ..self
        }
    }

    pub fn check(&self) -> Result<()> {
        if self.wins > self.games {
            return Err(Error::InvalidField("wins"));
        }
        if u64::from(self.best_streak) > self.games {
            return Err(Error::InvalidField("best_streak"));
        }
        if self.peak_class > SKILL_BANDS {
            return Err(Error::InvalidField("peak_class"));
        }
        Ok(())
    }
}

#[cfg(test)]
#[path = "awards/tests.rs"]
mod tests;
