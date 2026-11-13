//! Another player's card and the profile routes (`docs/design/PROFILE.md`):
//! the look-up that finds a card by token, friend, request or code, what a
//! card says of its player (their points, the counters behind their awards,
//! what they chose to show and how they play), the save of what the caller
//! chooses to show, and the search by name or friend code.
//!
//! A card is public: it always carries the account's points and the fighters
//! played (none when nothing is played), and is `null` only for a player with
//! no Discord account connected or who blocked the caller. Beyond the points
//! it says what its player chose to show and what the bridge knows of them:
//! the name, flag, main fighter, when they came to Ember, the award families
//! they wear and show, how they play, and the caller's own record and
//! friendship with them. It never carries a Discord account, an Ember ID or a
//! rating.
//!
//! Requests are strict and answers are tolerant. A request (`CardLookup`,
//! `CardSelector`, `CardSave`, `CardSearch`) refuses unknown members, so a
//! client's mistake is caught at the bridge. An answer (`CardAnswers`,
//! `PlayerCard`, `CardAccount`, `CardFighter`, `SearchAnswer`) ignores members
//! it does not know, so a field a later bridge adds does not break a client
//! already out; a closed enumeration travels as a small integer, and a value a
//! client does not know reads as not shown. A build asks only a bridge that
//! lists [`crate::levels::POINTS_FEATURE`] (and, for the routes and selectors
//! of the profile, [`PROFILES_FEATURE`]); an older bridge answers 404.
use serde::{Deserialize, Serialize};

use crate::{
    Error, Result,
    awards::{AwardCounts, is_family_id},
    encoding::{is_prefixed_id, is_single_line},
    friends::{PairRecord, parse_friend_code},
    levels::{FIGHTERS, MAX_TIER, is_zero},
    relay::is_region,
    rooms::{MAX_HOST_NAME, is_country, is_player_name},
    skill::{Listing, SkillBadge},
    wire_enum::wire_enum,
};

/// `POST` a `CardLookup`, answered with `CardAnswers`.
pub const LOOKUP_PATH: &str = "/v1/cards/lookup";
/// The capability a bridge with the profile routes offers, beside
/// [`crate::levels::POINTS_FEATURE`]: `PUT` [`CARDS_ME_PATH`], `POST` [`SEARCH_PATH`], the
/// selectors `request` and `code`, and the archive's `name`.
pub const PROFILES_FEATURE: &str = "profiles";
/// `PUT` a `CardSave`, answered with 204.
pub const CARDS_ME_PATH: &str = "/v1/cards/me";
/// `POST` a `CardSearch`, answered with `SearchAnswer`.
pub const SEARCH_PATH: &str = "/v1/cards/search";
/// The most award families a card shows.
pub const MAX_PINS: usize = 4;
/// The most players one search answers.
pub const MAX_SEARCH: usize = 20;
/// The shortest name a search takes, in bytes.
pub const MIN_SEARCH_NAME: usize = 2;
/// The highest `Style::device`: 1 Pad, 2 Stick, 3 Leverless, 4 Keyboard; 0 is
/// not shown.
pub const MAX_DEVICE: u8 = 4;
/// The highest `Style::link`: 1 Wired, 2 Wi-Fi; 0 is not shown.
pub const MAX_LINK: u8 = 2;
/// The bits a `Style::looking` may set: Casuals, Long sets, Ranked, Tournament
/// practice, New to the game, Happy to teach.
pub const LOOKING_BITS: u32 = 0b11_1111;
/// The most bits of `Style::looking` a player sets.
pub const MAX_LOOKING: u32 = 2;
/// The most selectors one look-up takes.
pub const MAX_LOOKUP: usize = 16;
/// What every card token starts with; the `1` names the token's format.
pub const CARD_PREFIX: &str = "c1.";
/// The most characters after the prefix. A token the bridge seals today has
/// 76; the rest is room to grow without a client update.
pub const MAX_CARD_BODY: usize = 96;
/// How long after it was issued a card token opens: 7 days.
pub const CARD_VALID_SECS: u64 = 7 * 24 * 60 * 60;

/// Whether `text` is shaped like a card token: the prefix, then 1 to
/// `MAX_CARD_BODY` characters of the unpadded base64url alphabet. Only the
/// bridge can tell whether one opens.
pub fn is_card_token(text: &str) -> bool {
    text.strip_prefix(CARD_PREFIX).is_some_and(|body| {
        !body.is_empty()
            && body.len() <= MAX_CARD_BODY
            && body
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
    })
}

fn check_family(id: &Option<String>) -> Result<()> {
    match id {
        Some(id) if !is_family_id(id) => Err(Error::InvalidField("family")),
        _ => Ok(()),
    }
}

/// One way to name a player in a look-up.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CardSelector {
    /// A card token from a public room's snapshot (`is_card_token`).
    Card(String),
    /// A friend id (`fr_`, `docs/design/FRIENDS.md`).
    Friend(String),
    /// A friend request the caller received (`rq_`). Only for a bridge that
    /// lists [`PROFILES_FEATURE`].
    Request(String),
    /// A friend code, as a player types one (`parse_friend_code`). Only for a
    /// bridge that lists [`PROFILES_FEATURE`].
    Code(String),
}

impl CardSelector {
    pub fn check(&self) -> Result<()> {
        match self {
            Self::Card(token) if is_card_token(token) => Ok(()),
            Self::Friend(id) if is_prefixed_id(id, "fr") => Ok(()),
            Self::Request(id) if is_prefixed_id(id, "rq") => Ok(()),
            Self::Code(code) if parse_friend_code(code).is_some() => Ok(()),
            Self::Card(_) => Err(Error::InvalidField("card")),
            Self::Friend(_) => Err(Error::InvalidField("friend")),
            Self::Request(_) => Err(Error::InvalidField("request")),
            Self::Code(_) => Err(Error::InvalidField("code")),
        }
    }
}

/// `POST /v1/cards/lookup`: 1 to `MAX_LOOKUP` selectors.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CardLookup {
    pub cards: Vec<CardSelector>,
}

impl CardLookup {
    pub fn check(&self) -> Result<()> {
        if self.cards.is_empty() || self.cards.len() > MAX_LOOKUP {
            return Err(Error::InvalidField("cards"));
        }
        self.cards.iter().try_for_each(CardSelector::check)
    }
}

wire_enum! {
    serde
    /// The caller's relation to the player a card is of. The bridge says
    /// `None` for a player who blocked the caller, and always before it
    /// offers friends.
    #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize)]
    pub enum Relation {
        None = "none",
        Friend = "friend",
        /// The caller asked them.
        Sent = "sent",
        /// They asked the caller.
        Received = "received",
        /// The caller blocked them.
        Blocked = "blocked",
    }
}

/// An unknown relation reads as `None`, so a relation a later bridge adds does
/// not fail the whole answer.
impl<'de> Deserialize<'de> for Relation {
    fn deserialize<D: serde::Deserializer<'de>>(
        deserializer: D,
    ) -> std::result::Result<Self, D::Error> {
        let name = String::deserialize(deserializer)?;
        Ok(Self::parse(&name).unwrap_or(Self::None))
    }
}

/// What a card shows of one fighter to someone else: the points and the
/// frame, never wins or games.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct CardFighter {
    /// 0 to `FIGHTERS` - 1.
    pub fighter: u8,
    /// At least 1.
    pub points: u64,
    /// The frame colour, 0 to `MAX_TIER`.
    pub tier: u8,
}

impl CardFighter {
    pub fn check(&self) -> Result<()> {
        if self.fighter >= FIGHTERS {
            return Err(Error::InvalidField("fighter"));
        }
        if self.points == 0 {
            return Err(Error::InvalidField("points"));
        }
        if self.tier > MAX_TIER {
            return Err(Error::InvalidField("tier"));
        }
        Ok(())
    }
}

/// What a card shows of the account's points to someone else: the points and
/// the frame. The games and wins behind them are in the card's `awards`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct CardAccount {
    /// 0 for an account with no counted game.
    pub points: u64,
    /// The frame colour, 0 to `MAX_TIER`.
    pub tier: u8,
}

impl CardAccount {
    pub fn check(&self) -> Result<()> {
        if self.tier > MAX_TIER {
            return Err(Error::InvalidField("tier"));
        }
        Ok(())
    }
}


/// The award families a player wears beside their name: the title's and the
/// icon's, as family ids. The viewer draws each family's current level (its
/// title, its icon in that level's tier), so a family the player has reached
/// no level of is not drawn.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Worn {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub title: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub icon: Option<String>,
}

impl Worn {
    pub fn is_empty(&self) -> bool {
        self.title.is_none() && self.icon.is_none()
    }

    pub fn check(&self) -> Result<()> {
        check_family(&self.title)?;
        check_family(&self.icon)
    }
}

/// How a player plays, as small numbers the viewer words: the device, the
/// connection, the relay region and what they are looking for. Every member
/// is absent at 0, and a value a reader does not know reads as not shown.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Style {
    /// 1 Pad, 2 Stick, 3 Leverless, 4 Keyboard; absent for not shown.
    #[serde(default, skip_serializing_if = "is_zero")]
    pub device: u8,
    /// 1 Wired, 2 Wi-Fi; absent for not shown.
    #[serde(default, skip_serializing_if = "is_zero")]
    pub link: u8,
    /// A relay region's code (`relay::REGIONS`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub region: Option<String>,
    /// Bits of `LOOKING_BITS`: 1 Casuals, 2 Long sets, 4 Ranked, 8 Tournament
    /// practice, 16 New to the game, 32 Happy to teach.
    #[serde(default, skip_serializing_if = "is_zero")]
    pub looking: u32,
}

/// The longest region code an answer carries.
const MAX_REGION_CODE: usize = 8;

impl Style {
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }

    /// What a bridge accepts to store: each number in its range, a region it
    /// knows, and at most `MAX_LOOKING` bits of what the player looks for.
    pub fn check_save(&self) -> Result<()> {
        if self.device > MAX_DEVICE {
            return Err(Error::InvalidField("device"));
        }
        if self.link > MAX_LINK {
            return Err(Error::InvalidField("link"));
        }
        if self.region.as_deref().is_some_and(|region| !is_region(region)) {
            return Err(Error::InvalidField("region"));
        }
        if self.looking & !LOOKING_BITS != 0 || self.looking.count_ones() > MAX_LOOKING {
            return Err(Error::InvalidField("looking"));
        }
        Ok(())
    }

    /// What a client accepts in an answer: a later bridge's device, link,
    /// region or bit is not refused, and reads as not shown.
    pub fn check(&self) -> Result<()> {
        let shaped = self.region.as_deref().is_none_or(|region| {
            !region.is_empty()
                && region.len() <= MAX_REGION_CODE
                && region
                    .bytes()
                    .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit())
        });
        if shaped {
            Ok(())
        } else {
            Err(Error::InvalidField("region"))
        }
    }
}

/// One player's card in a look-up answer. Each member after `awards` is
/// written only when there is something to say.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlayerCard {
    pub account: CardAccount,
    /// The fighters played, ascending by `fighter`, each once; empty when
    /// none is played. Always written.
    #[serde(default)]
    pub fighters: Vec<CardFighter>,
    pub relation: Relation,
    /// The counters behind the awards (`AwardCounts::shown`). Absent from a
    /// bridge that does not send them, and then every award reads as locked.
    #[serde(default, skip_serializing_if = "AwardCounts::is_empty")]
    pub awards: AwardCounts,
    /// The name their game last sent (`Listing`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    /// Their main fighter, 0 to `FIGHTERS` - 1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub main: Option<u8>,
    /// Their class, as a friends state shows one: absent while the bridge
    /// hides classes and for a player who does not take part in ranking.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub badge: Option<SkillBadge>,
    /// When they came to Ember, in unix seconds: the first second of that month.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub since: Option<u64>,
    /// The award families they wear.
    #[serde(default, skip_serializing_if = "Worn::is_empty")]
    pub worn: Worn,
    /// The award families they show, 0 to `MAX_PINS`; empty is Automatic.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub pins: Vec<String>,
    /// How they play.
    #[serde(default, skip_serializing_if = "Style::is_empty")]
    pub style: Style,
    /// The caller's record against them.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub versus: Option<PairRecord>,
    /// The friendship's id (`fr_`), when `relation` is `friend`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub friend: Option<String>,
}

impl PlayerCard {
    /// A card with the points of `account`, no fighters and nothing else.
    pub fn bare(account: CardAccount, relation: Relation) -> Self {
        Self {
            account,
            fighters: Vec::new(),
            relation,
            awards: AwardCounts::default(),
            name: None,
            country: None,
            main: None,
            badge: None,
            since: None,
            worn: Worn::default(),
            pins: Vec::new(),
            style: Style::default(),
            versus: None,
            friend: None,
        }
    }

    pub fn check(&self) -> Result<()> {
        self.account.check()?;
        self.awards.check()?;
        if self.fighters.len() > usize::from(FIGHTERS)
            || !self
                .fighters
                .windows(2)
                .all(|pair| pair[0].fighter < pair[1].fighter)
        {
            return Err(Error::InvalidField("fighters"));
        }
        self.fighters.iter().try_for_each(CardFighter::check)?;
        if self.name.as_deref().is_some_and(|name| !is_player_name(name)) {
            return Err(Error::InvalidField("name"));
        }
        if self.country.as_deref().is_some_and(|country| !is_country(country)) {
            return Err(Error::InvalidField("country"));
        }
        if self.main.is_some_and(|main| main >= FIGHTERS) {
            return Err(Error::InvalidField("main"));
        }
        if let Some(badge) = &self.badge {
            badge.check()?;
        }
        self.worn.check()?;
        if self.pins.len() > MAX_PINS || !self.pins.iter().all(|pin| is_family_id(pin)) {
            return Err(Error::InvalidField("pins"));
        }
        self.style.check()?;
        if let Some(versus) = &self.versus {
            versus.check()?;
        }
        if self.friend.as_deref().is_some_and(|id| !is_prefixed_id(id, "fr")) {
            return Err(Error::InvalidField("friend"));
        }
        Ok(())
    }
}

/// The answer to a `CardLookup`: one entry per selector, in order, `null`
/// for a selector that names nobody the caller may see.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct CardAnswers {
    pub cards: Vec<Option<PlayerCard>>,
}

impl CardAnswers {
    pub fn check(&self) -> Result<()> {
        if self.cards.is_empty() || self.cards.len() > MAX_LOOKUP {
            return Err(Error::InvalidField("cards"));
        }
        self.cards.iter().flatten().try_for_each(PlayerCard::check)
    }

    /// The answer's own rules, and that it answers `request` entry for entry.
    pub fn check_for(&self, request: &CardLookup) -> Result<()> {
        if self.cards.len() != request.cards.len() {
            return Err(Error::Mismatch("cards"));
        }
        self.check()
    }
}

/// `PUT /v1/cards/me`: what the caller's card shows, replacing the whole of
/// it. Answered with 204. The listing is the one the rankings take, and a
/// card saved without `main`, `worn`, `pins` or `style` says none of them.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CardSave {
    pub listing: Listing,
    /// 0 to `FIGHTERS` - 1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub main: Option<u8>,
    #[serde(default, skip_serializing_if = "Worn::is_empty")]
    pub worn: Worn,
    /// 0 to `MAX_PINS` distinct family ids; empty is Automatic.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub pins: Vec<String>,
    #[serde(default, skip_serializing_if = "Style::is_empty")]
    pub style: Style,
}

impl CardSave {
    pub fn check(&self) -> Result<()> {
        self.listing.check()?;
        if self.main.is_some_and(|main| main >= FIGHTERS) {
            return Err(Error::InvalidField("main"));
        }
        self.worn.check()?;
        let distinct = self
            .pins
            .iter()
            .enumerate()
            .all(|(at, pin)| !self.pins[..at].contains(pin));
        if self.pins.len() > MAX_PINS || !distinct || !self.pins.iter().all(|pin| is_family_id(pin))
        {
            return Err(Error::InvalidField("pins"));
        }
        self.style.check_save()
    }
}

/// `POST /v1/cards/search`: players by name or by friend code. Exactly one
/// of the two.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CardSearch {
    /// 2 to 32 bytes after trimming, on one line.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// A friend code as a player types one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub code: Option<String>,
}

impl CardSearch {
    /// `name`, trimmed, as a search reads it.
    pub fn trimmed_name(&self) -> Option<&str> {
        self.name.as_deref().map(str::trim)
    }

    pub fn check(&self) -> Result<()> {
        match (&self.name, &self.code) {
            (Some(_), None) => {
                let name = self.trimmed_name().unwrap_or_default();
                if name.len() >= MIN_SEARCH_NAME
                    && name.len() <= MAX_HOST_NAME
                    && is_single_line(name, MAX_HOST_NAME)
                {
                    Ok(())
                } else {
                    Err(Error::InvalidField("name"))
                }
            }
            (None, Some(code)) if parse_friend_code(code).is_some() => Ok(()),
            (None, Some(_)) => Err(Error::InvalidField("code")),
            _ => Err(Error::InvalidField("search")),
        }
    }
}

/// One player a search found: enough to list them and to open their card.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SearchHit {
    /// A fresh card token (`CARD_PREFIX`), valid `CARD_VALID_SECS`.
    pub card: String,
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub main: Option<u8>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub badge: Option<SkillBadge>,
    /// The first second of the month they came to Ember.
    pub since: u64,
}

impl SearchHit {
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
        if self.main.is_some_and(|main| main >= FIGHTERS) {
            return Err(Error::InvalidField("main"));
        }
        self.badge.as_ref().map_or(Ok(()), SkillBadge::check)
    }
}

/// The answer to a `CardSearch`: at most `MAX_SEARCH` players, and whether
/// more matched. A name's exact matches (any case) come first, then names
/// that begin with it, each most recently updated first.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SearchAnswer {
    pub players: Vec<SearchHit>,
    #[serde(default)]
    pub more: bool,
}

impl SearchAnswer {
    pub fn check(&self) -> Result<()> {
        if self.players.len() > MAX_SEARCH {
            return Err(Error::InvalidField("players"));
        }
        self.players.iter().try_for_each(SearchHit::check)
    }
}

#[cfg(test)]
#[path = "cards/tests.rs"]
mod tests;
