//! A player's public room class, as the bridge signs it into their room
//! ticket, and the ranking routes: a player's own class, their choice to take
//! part, and the board of classes (`docs/design/SKILL_BADGE.md`). The rating
//! behind a class never leaves the bridge.
use serde::{Deserialize, Serialize};

use crate::{Error, Result, encoding::is_false, rooms::is_player_name};

/// The capability a bridge with the ranking routes offers.
pub const RANKING_FEATURE: &str = "ranking";
/// `GET` the caller's `Standing`; `PUT` a `RankingChoice`, answered with it.
pub const STANDING_PATH: &str = "/v1/ranking/me";
/// `GET` the `Board`.
pub const BOARD_PATH: &str = "/v1/ranking/board";
/// The most players a board lists.
pub const MAX_BOARD: usize = 500;
/// Refusal reasons of the ranking routes: a choice needs a Discord account
/// to belong to, and taking part changes at most once a day.
pub const NO_ACCOUNT: &str = "no_discord_account";
pub const CHOICE_TOO_SOON: &str = "choice_too_soon";

/// The highest band of the skill badge. Bands are USF4's own online classes,
/// 1 (D) to 10 (Grand Master): D, D+, C, C+, B, B+, A, A+, Master, Grand Master.
pub const SKILL_BANDS: u8 = 10;

/// A player's class as a room shows it beside their name, or Ember's
/// developer mark in its place.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SkillBadge {
    /// 1 to `SKILL_BANDS`, or 0 before the player's first class (placing).
    pub band: u8,
    /// Whether the rating is sure enough to stand behind the band: false
    /// while a new player's rating forms and again after a long break.
    pub settled: bool,
    /// One of Ember's developers, whom the bridge's operator names: shown in
    /// place of a class, which it then leaves out (band 0). Only for a build
    /// that asks (`TicketRequest::developer_badge`), and written only when
    /// set, so every other badge is byte for byte what it was before.
    #[serde(default, skip_serializing_if = "is_false")]
    pub developer: bool,
}

impl SkillBadge {
    /// The developer mark.
    pub const DEVELOPER: Self = Self {
        band: 0,
        settled: false,
        developer: true,
    };

    /// A class, or placing at band 0.
    pub const fn class(band: u8, settled: bool) -> Self {
        Self {
            band,
            settled,
            developer: false,
        }
    }

    pub fn check(&self) -> Result<()> {
        let valid = self.band <= SKILL_BANDS
            && (self.band != 0 || !self.settled)
            && (!self.developer || *self == Self::DEVELOPER);
        if valid {
            Ok(())
        } else {
            Err(Error::InvalidField("badge"))
        }
    }
}

/// How the board lists a player: the name and flag their game shows, sent
/// by the game itself. A country is two upper-case letters; the game's own
/// table says which it draws a flag for.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Listing {
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
}

impl Listing {
    pub fn check(&self) -> Result<()> {
        if !is_player_name(&self.name) {
            return Err(Error::InvalidField("name"));
        }
        let country_valid = self.country.as_ref().is_none_or(|country| {
            country.len() == 2 && country.bytes().all(|byte| byte.is_ascii_uppercase())
        });
        if country_valid {
            Ok(())
        } else {
            Err(Error::InvalidField("country"))
        }
    }
}

/// `PUT /v1/ranking/me`: whether the caller's Discord account takes part in
/// ranking, and how the board lists it. It replaces the whole choice: a
/// player who takes part without a listing is ranked but not on the board,
/// and one who stops is listed nowhere.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RankingChoice {
    pub ranked: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub listing: Option<Listing>,
}

impl RankingChoice {
    pub fn check(&self) -> Result<()> {
        match &self.listing {
            Some(_) if !self.ranked => Err(Error::InvalidField("listing")),
            Some(listing) => listing.check(),
            None => Ok(()),
        }
    }
}

/// The caller's own place in ranking (`GET /v1/ranking/me`).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "ranking", rename_all = "snake_case")]
pub enum Standing {
    /// No Discord account is connected to the caller's Ember ID, and a
    /// rating belongs to one.
    NoAccount,
    /// The caller chose not to take part: no class, no board, no rated games.
    OptedOut {
        /// Seconds until the choice may change again, if it may not yet.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        change_in: Option<u64>,
    },
    Ranked {
        /// The class the caller's next room ticket carries; absent while the
        /// bridge keeps classes hidden.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        badge: Option<SkillBadge>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        listing: Option<Listing>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        change_in: Option<u64>,
    },
}

impl Standing {
    pub fn check(&self) -> Result<()> {
        match self {
            Self::Ranked { badge, listing, .. } => {
                badge.as_ref().map_or(Ok(()), SkillBadge::check)?;
                listing.as_ref().map_or(Ok(()), Listing::check)
            }
            Self::NoAccount | Self::OptedOut { .. } => Ok(()),
        }
    }
}

/// `GET /v1/ranking/board`: the listed players with a settled class, the
/// highest class first.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Board {
    pub players: Vec<BoardEntry>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoardEntry {
    pub listing: Listing,
    /// 1 to `SKILL_BANDS`.
    pub band: u8,
    /// The caller's own entry.
    #[serde(default, skip_serializing_if = "is_false")]
    pub own: bool,
}

impl Board {
    pub fn check(&self) -> Result<()> {
        if self.players.len() > MAX_BOARD {
            return Err(Error::InvalidField("players"));
        }
        let ordered = self
            .players
            .windows(2)
            .all(|pair| pair[0].band >= pair[1].band);
        if !ordered {
            return Err(Error::InvalidField("players"));
        }
        for entry in &self.players {
            entry.listing.check()?;
            if !(1..=SKILL_BANDS).contains(&entry.band) {
                return Err(Error::InvalidField("band"));
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_badge_is_a_class_or_none_yet() {
        let badge = SkillBadge::class;
        for valid in [
            badge(0, false),
            badge(1, true),
            badge(SKILL_BANDS, false),
            SkillBadge::DEVELOPER,
        ] {
            assert!(valid.check().is_ok(), "{valid:?}");
        }
        // No class yet, so nothing to be settled about; and no class past the last.
        // The developer mark leaves the class out.
        let developer_with_class = SkillBadge {
            developer: true,
            ..badge(4, true)
        };
        for invalid in [
            badge(0, true),
            badge(SKILL_BANDS + 1, true),
            developer_with_class,
        ] {
            assert_eq!(
                invalid.check(),
                Err(Error::InvalidField("badge")),
                "{invalid:?}"
            );
        }
    }

    #[test]
    fn a_standing_reads_as_one_of_three_shapes() {
        let read = |text: &str| serde_json::from_str::<Standing>(text);
        assert_eq!(
            read(r#"{"ranking":"no_account"}"#).unwrap(),
            Standing::NoAccount
        );
        assert_eq!(
            read(r#"{"ranking":"opted_out","change_in":60}"#).unwrap(),
            Standing::OptedOut {
                change_in: Some(60)
            }
        );
        let ranked = read(
            r#"{"ranking":"ranked","badge":{"band":4,"settled":true},"listing":{"name":"Player 1","country":"GB"}}"#,
        )
        .unwrap();
        assert!(ranked.check().is_ok());
        assert_eq!(
            serde_json::to_string(&Standing::Ranked {
                badge: None,
                listing: None,
                change_in: None
            })
            .unwrap(),
            r#"{"ranking":"ranked"}"#
        );
        // An answer ignores a member it does not know, but not a state.
        assert_eq!(
            read(r#"{"ranking":"opted_out","badge":null}"#).unwrap(),
            Standing::OptedOut { change_in: None }
        );
        assert!(read(r#"{"ranking":"top"}"#).is_err());
    }

    #[test]
    fn a_listing_is_a_player_name_and_maybe_a_country() {
        let listing = |name: &str, country: Option<&str>| Listing {
            name: name.to_owned(),
            country: country.map(str::to_owned),
        };
        for valid in [listing("Player 1", Some("JP")), listing("Player 2", None)] {
            assert!(valid.check().is_ok(), "{valid:?}");
        }
        for invalid in [
            listing("", None),
            listing(" Player 1", None),
            listing(&"x".repeat(33), None),
            listing("Player 1", Some("jp")),
            listing("Player 1", Some("JPN")),
        ] {
            assert!(invalid.check().is_err(), "{invalid:?}");
        }
        // Only a player who takes part is listed.
        let opted_out = RankingChoice {
            ranked: false,
            listing: Some(listing("Player 1", None)),
        };
        assert_eq!(opted_out.check(), Err(Error::InvalidField("listing")));
    }

    #[test]
    fn a_board_lists_classes_highest_first() {
        let entry = |band| BoardEntry {
            listing: Listing {
                name: "Player 1".to_owned(),
                country: None,
            },
            band,
            own: false,
        };
        let board = |bands: &[u8]| Board {
            players: bands.iter().copied().map(entry).collect(),
        };
        assert!(board(&[SKILL_BANDS, 4, 4, 1]).check().is_ok());
        assert!(board(&[4, 5]).check().is_err());
        assert!(board(&[0]).check().is_err());
        assert_eq!(
            serde_json::to_string(&entry(3)).unwrap(),
            r#"{"listing":{"name":"Player 1"},"band":3}"#
        );
    }
}
