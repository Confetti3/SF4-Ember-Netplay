//! Points: a player's own (`docs/design/FIGHTER_LEVELS.md`). Every Discord
//! account has account points (BP) and points for each fighter it played (FP).
//! Both are a grind value, never skill: games plus wins. The tier thresholds
//! behind the frame colour live only in the bridge, so these types carry what
//! a client needs to draw the points and their progress, and nothing of the
//! rating.
//!
//! Another player's card, the look-up that finds it and the profile routes
//! are in `cards`, the counters behind the awards in `awards`; their types are
//! re-exported here, so `levels::PlayerCard` and `levels::AwardCounts` name
//! them as they always did.
//!
//! An answer (`OwnLevels`, `OwnAccount`, `OwnFighter`) ignores members it does
//! not know, so a field a later bridge adds does not break a client already
//! out. A build asks only a bridge that lists [`POINTS_FEATURE`]; an older
//! bridge answers 404.
use serde::{Deserialize, Serialize};

use crate::{
    Error, Result,
    cards::is_card_token,
    rooms::{is_country, is_player_name},
    skill::SkillBadge,
};

pub use crate::awards::{AwardCounts, MAX_FAMILY_ID, OPPONENT_MARKS, RIVAL_MARKS, is_family_id, mark};
pub use crate::cards::*;

/// The capability a bridge with the levels routes offers.
pub const POINTS_FEATURE: &str = "points";
/// `GET` the caller's `OwnLevels`.
pub const POINTS_PATH: &str = "/v1/points/me";
/// Fighters a player can have points for: 0 to 43, USF4's roster.
pub const FIGHTERS: u8 = 44;
/// The highest tier: tiers are 0 to `MAX_TIER`, Ash to Ember.
pub const MAX_TIER: u8 = 4;

/// The caller's own points (`GET /v1/points/me`).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "points", rename_all = "snake_case")]
pub enum OwnLevels {
    /// No Discord account is connected to the caller's Ember ID, and points
    /// belong to one.
    NoAccount {},
    Counted {
        /// The account's points, always present.
        account: OwnAccount,
        /// The fighters played, ascending by `fighter`, each once.
        fighters: Vec<OwnFighter>,
        /// The opponent the caller played the most (docs/design/PROFILE.md,
        /// "the most played opponent"); absent before a counted game, when
        /// no opponent can be shown, and from a bridge that does not send it.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        rival: Option<RivalCard>,
    },
}

/// The caller's most played opponent: enough to list them and to open their
/// card, and the caller's record against them. Only in the caller's own
/// answer, never on a card, so no card names a third party.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct RivalCard {
    /// A fresh card token (`CARD_PREFIX`), valid `CARD_VALID_SECS`.
    pub card: String,
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub badge: Option<SkillBadge>,
    /// Counted games the caller won against them.
    pub won: u64,
    /// Counted games they won against the caller. `won + lost` is the games
    /// the two played, at least 1.
    pub lost: u64,
}

impl RivalCard {
    pub fn check(&self) -> Result<()> {
        if !is_card_token(&self.card) {
            return Err(Error::InvalidField("card"));
        }
        if !is_player_name(&self.name) {
            return Err(Error::InvalidField("name"));
        }
        if self.country.as_deref().is_some_and(|country| !is_country(country)) {
            return Err(Error::InvalidField("country"));
        }
        if self.won.saturating_add(self.lost) == 0 {
            return Err(Error::InvalidField("won"));
        }
        self.badge.as_ref().map_or(Ok(()), SkillBadge::check)
    }
}

/// The tier frame of a points count: the colour, where it begins and where
/// the next begins. Shared by the account's and each fighter's points.
fn check_frame(points: u64, tier: u8, floor: u64, next: Option<u64>) -> Result<()> {
    if tier > MAX_TIER {
        return Err(Error::InvalidField("tier"));
    }
    if floor > points || (tier == 0 && floor != 0) {
        return Err(Error::InvalidField("floor"));
    }
    let next_valid = match next {
        None => tier == MAX_TIER,
        Some(next) => tier < MAX_TIER && points < next,
    };
    if next_valid {
        Ok(())
    } else {
        Err(Error::InvalidField("next"))
    }
}

/// `games + wins`, saturating: a win adds 2, a loss 1.
fn check_counts(points: u64, games: u64, wins: u64) -> Result<()> {
    if wins > games {
        return Err(Error::InvalidField("wins"));
    }
    if points != games.saturating_add(wins) {
        return Err(Error::InvalidField("points"));
    }
    Ok(())
}

/// The caller's account points (BP): every counted game, whether or not it
/// named fighters, so it can be 0 with no games.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct OwnAccount {
    /// `games + wins`.
    pub points: u64,
    /// The counted games and wins, and the counters beside them that the
    /// awards are reached from (`AwardCounts`). Flattened: on the wire they
    /// are members of the account, as they always were.
    #[serde(flatten)]
    pub awards: AwardCounts,
    /// The frame colour, 0 to `MAX_TIER`, on the account's own thresholds.
    pub tier: u8,
    /// As `OwnFighter::floor`.
    pub floor: u64,
    /// As `OwnFighter::next`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub next: Option<u64>,
    /// When the first game was counted (unix seconds); absent while `games`
    /// is 0.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub since: Option<u64>,
}

/// Whether a number is its default, for the members a answer leaves out at 0.
pub(crate) fn is_zero<T: Default + PartialEq>(value: &T) -> bool {
    *value == T::default()
}

impl OwnAccount {
    pub fn check(&self) -> Result<()> {
        check_counts(self.points, self.awards.games, self.awards.wins)?;
        self.awards.check()?;
        if self.since.is_some() != (self.awards.games > 0) {
            return Err(Error::InvalidField("since"));
        }
        check_frame(self.points, self.tier, self.floor, self.next)
    }
}

/// One played fighter's points and what they were counted from.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct OwnFighter {
    /// 0 to `FIGHTERS` - 1.
    pub fighter: u8,
    /// `games + wins`: a win adds 2, a loss 1.
    pub points: u64,
    /// Counted games, at least 1.
    pub games: u64,
    /// Counted wins, at most `games`.
    pub wins: u64,
    /// The frame colour, 0 to `MAX_TIER`.
    pub tier: u8,
    /// The points where the tier begins, at most `points`; 0 at tier 0. Always
    /// present, so a client can draw progress from `floor` to `next`.
    pub floor: u64,
    /// The points where the next tier begins, above `points`; absent exactly
    /// at `MAX_TIER`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub next: Option<u64>,
    /// When the fighter was first counted (unix seconds).
    pub since: u64,
}

impl OwnFighter {
    pub fn check(&self) -> Result<()> {
        if self.fighter >= FIGHTERS {
            return Err(Error::InvalidField("fighter"));
        }
        if self.games == 0 {
            return Err(Error::InvalidField("games"));
        }
        check_counts(self.points, self.games, self.wins)?;
        check_frame(self.points, self.tier, self.floor, self.next)
    }
}

impl OwnLevels {
    pub fn check(&self) -> Result<()> {
        let Self::Counted {
            account,
            fighters,
            rival,
        } = self
        else {
            return Ok(());
        };
        account.check()?;
        rival.as_ref().map_or(Ok(()), RivalCard::check)?;
        if fighters.len() > usize::from(FIGHTERS)
            || !fighters
                .windows(2)
                .all(|pair| pair[0].fighter < pair[1].fighter)
        {
            return Err(Error::InvalidField("fighters"));
        }
        fighters.iter().try_for_each(OwnFighter::check)
    }
}

#[cfg(test)]
#[path = "levels/tests.rs"]
pub(crate) mod tests;
