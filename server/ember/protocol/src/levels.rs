//! Fighter levels: a player's own levels, and the card look-up that shows
//! other players' (`docs/design/FIGHTER_LEVELS.md`). A level is a grind value
//! for one fighter, never skill; the curve behind it lives only in the
//! bridge, so these types carry what a client needs to draw a level and its
//! progress, and nothing of the rating.
//!
//! Every type refuses unknown members: a build asks only a bridge that lists
//! [`LEVELS_FEATURE`], and an older bridge answers 404.
use serde::{Deserialize, Serialize};

use crate::{
    Error, Result,
    encoding::{is_false, is_prefixed_id},
    wire_enum::wire_enum,
};

/// The capability a bridge with the levels routes offers.
pub const LEVELS_FEATURE: &str = "levels";
/// `GET` the caller's `OwnLevels`.
pub const LEVELS_PATH: &str = "/v1/levels/me";
/// `POST` a `CardLookup`, answered with `CardAnswers`.
pub const LOOKUP_PATH: &str = "/v1/cards/lookup";
/// Fighters a player can have a level for: 0 to 43, USF4's roster.
pub const FIGHTERS: u8 = 44;
/// The highest level.
pub const MAX_LEVEL: u8 = 60;
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

fn level_valid(level: u8) -> bool {
    (1..=MAX_LEVEL).contains(&level)
}

/// The caller's own levels (`GET /v1/levels/me`).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "levels", rename_all = "snake_case", deny_unknown_fields)]
pub enum OwnLevels {
    /// No Discord account is connected to the caller's Ember ID, and levels
    /// belong to one.
    NoAccount {},
    Counted {
        /// Others see this account's levels. Written only when true.
        #[serde(default, skip_serializing_if = "is_false")]
        shown: bool,
        /// The fighters played, ascending by `fighter`, each once.
        fighters: Vec<OwnFighter>,
    },
}

/// One played fighter's level and what it was counted from.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct OwnFighter {
    /// 0 to `FIGHTERS` - 1.
    pub fighter: u8,
    /// 1 to `MAX_LEVEL`.
    pub level: u8,
    /// Counted games, at least 1.
    pub games: u64,
    /// Counted wins, at most `games`.
    pub wins: u64,
    /// The points of this level: at most `games + wins`.
    pub floor: u64,
    /// The points of the next level, above `games + wins`; absent at
    /// `MAX_LEVEL`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub next: Option<u64>,
    /// When the fighter was first counted (unix seconds).
    pub since: u64,
}

impl OwnFighter {
    pub fn check(&self) -> Result<()> {
        let points = self.games.saturating_add(self.wins);
        if self.fighter >= FIGHTERS {
            return Err(Error::InvalidField("fighter"));
        }
        if !level_valid(self.level) {
            return Err(Error::InvalidField("level"));
        }
        if self.games == 0 {
            return Err(Error::InvalidField("games"));
        }
        if self.wins > self.games {
            return Err(Error::InvalidField("wins"));
        }
        if self.floor > points {
            return Err(Error::InvalidField("floor"));
        }
        let next_valid = match self.next {
            None => self.level == MAX_LEVEL,
            Some(next) => self.level < MAX_LEVEL && points < next,
        };
        if next_valid {
            Ok(())
        } else {
            Err(Error::InvalidField("next"))
        }
    }
}

impl OwnLevels {
    pub fn check(&self) -> Result<()> {
        let Self::Counted { fighters, .. } = self else {
            return Ok(());
        };
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

/// One way to name a player in a look-up.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CardSelector {
    /// A card token from a public room's snapshot (`is_card_token`).
    Card(String),
    /// A friend id (`fr_`, `docs/design/FRIENDS.md`).
    Friend(String),
}

impl CardSelector {
    pub fn check(&self) -> Result<()> {
        match self {
            Self::Card(token) if is_card_token(token) => Ok(()),
            Self::Friend(id) if is_prefixed_id(id, "fr") => Ok(()),
            Self::Card(_) => Err(Error::InvalidField("card")),
            Self::Friend(_) => Err(Error::InvalidField("friend")),
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
    #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
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

/// What a card shows of one fighter to someone else: never wins, points or
/// the floor.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CardFighter {
    /// 0 to `FIGHTERS` - 1.
    pub fighter: u8,
    /// 1 to `MAX_LEVEL`.
    pub level: u8,
    /// Counted games, at least 1.
    pub games: u64,
}

impl CardFighter {
    pub fn check(&self) -> Result<()> {
        if self.fighter >= FIGHTERS {
            return Err(Error::InvalidField("fighter"));
        }
        if !level_valid(self.level) {
            return Err(Error::InvalidField("level"));
        }
        if self.games == 0 {
            return Err(Error::InvalidField("games"));
        }
        Ok(())
    }
}

/// One player's card in a look-up answer.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PlayerCard {
    /// The fighters played, ascending by `fighter`, each once; absent when
    /// the player does not show levels.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fighters: Option<Vec<CardFighter>>,
    pub relation: Relation,
}

impl PlayerCard {
    pub fn check(&self) -> Result<()> {
        let Some(fighters) = &self.fighters else {
            return Ok(());
        };
        if fighters.len() > usize::from(FIGHTERS)
            || !fighters
                .windows(2)
                .all(|pair| pair[0].fighter < pair[1].fighter)
        {
            return Err(Error::InvalidField("fighters"));
        }
        fighters.iter().try_for_each(CardFighter::check)
    }
}

/// The answer to a `CardLookup`: one entry per selector, in order, `null`
/// for a selector that names nobody the caller may see.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
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

#[cfg(test)]
mod tests {
    use serde::de::DeserializeOwned;
    use serde_json::{Value, json};

    use super::*;

    const FRIEND: &str = "fr_11111111-1111-4111-8111-111111111111";

    fn read<T: DeserializeOwned>(value: &Value) -> T {
        serde_json::from_value(value.clone()).unwrap()
    }

    /// Adds a member the type does not have at `pointer` and expects a refusal.
    fn refuses_extra<T: DeserializeOwned>(value: &Value, pointer: &str) {
        let mut value = value.clone();
        value
            .pointer_mut(pointer)
            .unwrap()
            .as_object_mut()
            .unwrap()
            .insert("extra".into(), json!(1));
        assert!(
            serde_json::from_value::<T>(value).is_err(),
            "extra member at {pointer:?}"
        );
    }

    fn own(
        fighter: u8,
        level: u8,
        games: u64,
        wins: u64,
        floor: u64,
        next: Option<u64>,
    ) -> OwnFighter {
        OwnFighter {
            fighter,
            level,
            games,
            wins,
            floor,
            next,
            since: 1_729_468_800,
        }
    }

    #[test]
    fn the_own_levels_in_the_design_round_trip() {
        let none = json!({"levels": "no_account"});
        assert_eq!(read::<OwnLevels>(&none), OwnLevels::NoAccount {});
        assert_eq!(serde_json::to_value(OwnLevels::NoAccount {}).unwrap(), none);

        let counted = json!({"levels": "counted", "shown": true, "fighters": [
            {"fighter": 3, "level": 42, "games": 1000, "wins": 900,
             "floor": 1845, "next": 1932, "since": 1729468800}]});
        let levels: OwnLevels = read(&counted);
        assert_eq!(
            levels,
            OwnLevels::Counted {
                shown: true,
                fighters: vec![own(3, 42, 1000, 900, 1845, Some(1932))],
            }
        );
        assert!(levels.check().is_ok());
        assert_eq!(serde_json::to_value(&levels).unwrap(), counted);

        // `shown` is written only when true, and `next` only below the top.
        let hidden = OwnLevels::Counted {
            shown: false,
            fighters: vec![own(43, 60, 2480, 1237, 3717, None)],
        };
        assert!(hidden.check().is_ok());
        assert_eq!(
            serde_json::to_string(&hidden).unwrap(),
            r#"{"levels":"counted","fighters":[{"fighter":43,"level":60,"games":2480,"wins":1237,"floor":3717,"since":1729468800}]}"#
        );
        let nobody = OwnLevels::Counted {
            shown: false,
            fighters: vec![],
        };
        assert!(nobody.check().is_ok());
        assert_eq!(
            serde_json::to_string(&nobody).unwrap(),
            r#"{"levels":"counted","fighters":[]}"#
        );
    }

    #[test]
    fn own_levels_refuse_unknown_members_and_unknown_states() {
        let none = json!({"levels": "no_account"});
        refuses_extra::<OwnLevels>(&none, "");
        let counted = json!({"levels": "counted", "fighters": [
            {"fighter": 3, "level": 1, "games": 1, "wins": 0, "floor": 0, "next": 5, "since": 1}]});
        refuses_extra::<OwnLevels>(&counted, "");
        refuses_extra::<OwnLevels>(&counted, "/fighters/0");
        assert!(serde_json::from_value::<OwnLevels>(json!({"levels": "top"})).is_err());
        assert!(serde_json::from_value::<OwnLevels>(json!({"fighters": []})).is_err());
        // A member that is missing is refused, not defaulted.
        assert!(serde_json::from_value::<OwnLevels>(json!({"levels": "counted"})).is_err());
        assert!(
            serde_json::from_value::<OwnFighter>(
                json!({"fighter": 3, "level": 1, "games": 1, "wins": 0, "since": 1})
            )
            .is_err()
        );
    }

    #[test]
    fn a_fighter_level_is_checked_against_its_own_counts() {
        let good = own(3, 42, 1000, 900, 1845, Some(1932));
        assert!(good.check().is_ok());
        // The first level and the last.
        assert!(own(0, 1, 1, 0, 0, Some(5)).check().is_ok());
        assert!(own(43, 60, 3000, 1000, 3717, None).check().is_ok());
        // Exactly the floor, and one point under the next level.
        assert!(own(3, 2, 3, 2, 5, Some(6)).check().is_ok());

        let refused = [
            ("fighter", own(44, 1, 1, 0, 0, Some(5))),
            ("level", own(3, 0, 1, 0, 0, Some(5))),
            ("level", own(3, 61, 1, 0, 0, None)),
            ("games", own(3, 1, 0, 0, 0, Some(5))),
            ("wins", own(3, 1, 4, 5, 0, Some(20))),
            ("floor", own(3, 2, 3, 1, 5, Some(9))),
            ("next", own(3, 2, 3, 2, 5, Some(5))),
            ("next", own(3, 2, 3, 2, 5, Some(4))),
            // A level below the top has a next level; the top has none.
            ("next", own(3, 2, 3, 2, 5, None)),
            ("next", own(3, 60, 3000, 1000, 3717, Some(4000))),
        ];
        for (field, fighter) in refused {
            assert_eq!(
                fighter.check(),
                Err(Error::InvalidField(field)),
                "{fighter:?}"
            );
        }
        // Counts near the end of the range do not overflow.
        let huge = own(3, 60, u64::MAX, u64::MAX, 0, None);
        assert!(huge.check().is_ok());
    }

    #[test]
    fn own_fighters_are_ascending_unique_and_at_most_44() {
        let at = |fighter: u8| own(fighter, 1, 1, 0, 0, Some(5));
        let counted = |ids: &[u8]| OwnLevels::Counted {
            shown: false,
            fighters: ids.iter().copied().map(at).collect(),
        };
        assert!(counted(&[0, 1, 43]).check().is_ok());
        let all: Vec<u8> = (0..FIGHTERS).collect();
        assert!(counted(&all).check().is_ok());
        for ids in [vec![1, 0], vec![3, 3], vec![0, 44]] {
            assert!(counted(&ids).check().is_err(), "{ids:?}");
        }
        let mut too_many = all;
        too_many.push(43);
        assert_eq!(
            counted(&too_many).check(),
            Err(Error::InvalidField("fighters"))
        );
        assert!(OwnLevels::NoAccount {}.check().is_ok());
    }

    #[test]
    fn card_tokens_are_a_prefix_and_up_to_96_base64url_characters() {
        let seventy_six = format!("c1.{}", "aZ09-_".repeat(13).get(..76).unwrap());
        assert!(is_card_token(&seventy_six));
        assert!(is_card_token("c1.a"));
        assert!(is_card_token(&format!("c1.{}", "A".repeat(MAX_CARD_BODY))));
        for bad in [
            "",
            "c1.",
            "c1",
            "c2.abc",
            "C1.abc",
            "abc",
            "c1.a b",
            "c1.a=",
            "c1.a+b",
            "c1.a/b",
            "c1.\u{e9}",
            "c1.a\n",
        ] {
            assert!(!is_card_token(bad), "{bad:?}");
        }
        assert!(!is_card_token(&format!(
            "c1.{}",
            "A".repeat(MAX_CARD_BODY + 1)
        )));
    }

    #[test]
    fn a_look_up_takes_one_to_sixteen_tokens_or_friend_ids() {
        let wire = json!({"cards": [{"card": "c1.abc"}, {"friend": FRIEND}]});
        let lookup: CardLookup = read(&wire);
        assert_eq!(
            lookup.cards,
            vec![
                CardSelector::Card("c1.abc".into()),
                CardSelector::Friend(FRIEND.into())
            ]
        );
        assert!(lookup.check().is_ok());
        assert_eq!(serde_json::to_value(&lookup).unwrap(), wire);

        let cards = |count: usize| CardLookup {
            cards: vec![CardSelector::Card("c1.abc".into()); count],
        };
        assert!(cards(1).check().is_ok());
        assert!(cards(MAX_LOOKUP).check().is_ok());
        assert_eq!(cards(0).check(), Err(Error::InvalidField("cards")));
        assert_eq!(
            cards(MAX_LOOKUP + 1).check(),
            Err(Error::InvalidField("cards"))
        );
        assert_eq!(
            CardSelector::Card("fr_x".into()).check(),
            Err(Error::InvalidField("card"))
        );
        for friend in ["c1.abc", "rq_11111111-1111-4111-8111-111111111111", "fr_x"] {
            assert_eq!(
                CardSelector::Friend(friend.into()).check(),
                Err(Error::InvalidField("friend")),
                "{friend}"
            );
        }

        // A selector names one thing, and nothing it does not know.
        refuses_extra::<CardLookup>(&wire, "");
        for bad in [
            json!({"cards": [{"card": "c1.abc", "friend": FRIEND}]}),
            json!({"cards": [{"card": "c1.abc", "extra": 1}]}),
            json!({"cards": [{"code": "c1.abc"}]}),
            json!({"cards": [{}]}),
            json!({"cards": [{"card": 1}]}),
        ] {
            assert!(
                serde_json::from_value::<CardLookup>(bad.clone()).is_err(),
                "{bad}"
            );
        }
    }

    #[test]
    fn a_look_up_answer_has_one_entry_per_selector() {
        let wire = json!({"cards": [
            {"fighters": [{"fighter": 3, "level": 31, "games": 612}], "relation": "none"},
            null]});
        let answers: CardAnswers = read(&wire);
        assert!(answers.check().is_ok());
        assert_eq!(serde_json::to_value(&answers).unwrap(), wire);
        assert_eq!(answers.cards[1], None);

        let request = CardLookup {
            cards: vec![
                CardSelector::Card("c1.abc".into()),
                CardSelector::Friend(FRIEND.into()),
            ],
        };
        assert!(answers.check_for(&request).is_ok());
        let short = CardLookup {
            cards: request.cards[..1].to_vec(),
        };
        assert_eq!(answers.check_for(&short), Err(Error::Mismatch("cards")));

        // A player who does not show levels has no `fighters` at all, which
        // is not the same as showing none.
        let hidden = PlayerCard {
            fighters: None,
            relation: Relation::Blocked,
        };
        assert_eq!(
            serde_json::to_string(&hidden).unwrap(),
            r#"{"relation":"blocked"}"#
        );
        let empty = PlayerCard {
            fighters: Some(vec![]),
            relation: Relation::Sent,
        };
        assert_eq!(
            serde_json::to_string(&empty).unwrap(),
            r#"{"fighters":[],"relation":"sent"}"#
        );
        assert!(hidden.check().is_ok() && empty.check().is_ok());

        let empty_answer = CardAnswers { cards: vec![] };
        assert_eq!(empty_answer.check(), Err(Error::InvalidField("cards")));
        let long = CardAnswers {
            cards: vec![None; MAX_LOOKUP + 1],
        };
        assert_eq!(long.check(), Err(Error::InvalidField("cards")));

        refuses_extra::<CardAnswers>(&wire, "");
        refuses_extra::<CardAnswers>(&wire, "/cards/0");
        refuses_extra::<CardAnswers>(&wire, "/cards/0/fighters/0");
        // Wins, points and the floor are never on another player's card.
        let mut with_wins = wire.clone();
        with_wins["cards"][0]["fighters"][0]["wins"] = json!(3);
        assert!(serde_json::from_value::<CardAnswers>(with_wins).is_err());
    }

    #[test]
    fn a_card_fighter_is_checked_and_listed_in_order() {
        let fighter = |fighter: u8, level: u8, games: u64| CardFighter {
            fighter,
            level,
            games,
        };
        let card = |fighters: Vec<CardFighter>| PlayerCard {
            fighters: Some(fighters),
            relation: Relation::None,
        };
        assert!(
            card(vec![fighter(0, 1, 1), fighter(43, 60, 9)])
                .check()
                .is_ok()
        );
        for bad in [
            fighter(44, 1, 1),
            fighter(3, 0, 1),
            fighter(3, 61, 1),
            fighter(3, 1, 0),
        ] {
            assert!(bad.check().is_err(), "{bad:?}");
            assert!(card(vec![bad]).check().is_err(), "{bad:?}");
        }
        for unordered in [
            vec![fighter(5, 1, 1), fighter(4, 1, 1)],
            vec![fighter(5, 1, 1), fighter(5, 2, 1)],
        ] {
            assert_eq!(
                card(unordered).check(),
                Err(Error::InvalidField("fighters"))
            );
        }
    }

    #[test]
    fn relations_use_the_documents_names() {
        for (relation, name) in [
            (Relation::None, "none"),
            (Relation::Friend, "friend"),
            (Relation::Sent, "sent"),
            (Relation::Received, "received"),
            (Relation::Blocked, "blocked"),
        ] {
            assert_eq!(relation.as_str(), name);
            assert_eq!(Relation::parse(name), Some(relation));
            assert_eq!(serde_json::to_value(relation).unwrap(), name);
        }
        assert_eq!(Relation::ALL.len(), 5);
        assert!(serde_json::from_value::<Relation>(json!("blocked_by")).is_err());
    }
}
