use serde::de::DeserializeOwned;
use serde_json::{Value, json};

use super::*;

pub(crate) fn read<T: DeserializeOwned>(value: &Value) -> T {
    serde_json::from_value(value.clone()).unwrap()
}

/// Adds a member the type does not have at `pointer` and expects `T` to
/// read the value as it reads the one without it.
pub(crate) fn ignores_extra<T: DeserializeOwned + PartialEq + std::fmt::Debug>(
    value: &Value,
    pointer: &str,
) {
    let mut extended = value.clone();
    extended
        .pointer_mut(pointer)
        .unwrap()
        .as_object_mut()
        .unwrap()
        .insert("extra".into(), json!(1));
    assert_eq!(read::<T>(&extended), read::<T>(value), "{pointer:?}");
}

/// Adds a member the type does not have at `pointer` and expects a refusal.
pub(crate) fn refuses_extra<T: DeserializeOwned>(value: &Value, pointer: &str) {
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
    games: u64,
    wins: u64,
    tier: u8,
    floor: u64,
    next: Option<u64>,
) -> OwnFighter {
    OwnFighter {
        fighter,
        points: games.saturating_add(wins),
        games,
        wins,
        tier,
        floor,
        next,
        since: 1_729_468_800,
    }
}

fn account(games: u64, wins: u64, tier: u8, floor: u64, next: Option<u64>) -> OwnAccount {
    OwnAccount {
        points: games.saturating_add(wins),
        awards: AwardCounts {
            games,
            wins,
            ..AwardCounts::default()
        },
        tier,
        floor,
        next,
        since: (games > 0).then_some(1_729_468_800),
    }
}

#[test]
fn the_own_points_in_the_design_round_trip() {
    let none = json!({"points": "no_account"});
    assert_eq!(read::<OwnLevels>(&none), OwnLevels::NoAccount {});
    assert_eq!(serde_json::to_value(OwnLevels::NoAccount {}).unwrap(), none);

    let counted = json!({"points": "counted",
        "account": {"points": 13, "games": 9, "wins": 4, "tier": 0, "floor": 0,
                    "next": 500, "since": 1729468800},
        "fighters": [
        {"fighter": 3, "points": 1986, "games": 1284, "wins": 702,
         "tier": 3, "floor": 1500, "next": 2500, "since": 1729468800}]});
    let levels: OwnLevels = read(&counted);
    assert_eq!(
        levels,
        OwnLevels::Counted {
            account: account(9, 4, 0, 0, Some(500)),
            fighters: vec![own(3, 1284, 702, 3, 1500, Some(2500))],
            rival: None,
        }
    );
    assert!(levels.check().is_ok());
    assert_eq!(serde_json::to_value(&levels).unwrap(), counted);

    // `next` is written only below the top, and `since` only once a game
    // is counted.
    let top = OwnLevels::Counted {
        account: account(9000, 2000, 4, 10_000, None),
        fighters: vec![own(43, 2480, 1237, 4, 2500, None)],
        rival: None,
    };
    assert!(top.check().is_ok());
    assert_eq!(
        serde_json::to_string(&top).unwrap(),
        r#"{"points":"counted","account":{"points":11000,"games":9000,"wins":2000,"tier":4,"floor":10000,"since":1729468800},"fighters":[{"fighter":43,"points":3717,"games":2480,"wins":1237,"tier":4,"floor":2500,"since":1729468800}]}"#
    );
    let nobody = OwnLevels::Counted {
        account: account(0, 0, 0, 0, Some(500)),
        fighters: vec![],
        rival: None,
    };
    assert!(nobody.check().is_ok());
    assert_eq!(
        serde_json::to_string(&nobody).unwrap(),
        r#"{"points":"counted","account":{"points":0,"games":0,"wins":0,"tier":0,"floor":0,"next":500},"fighters":[]}"#
    );
}

#[test]
fn own_points_ignore_unknown_members_and_refuse_unknown_states() {
    let none = json!({"points": "no_account"});
    ignores_extra::<OwnLevels>(&none, "");
    let counted = json!({"points": "counted",
        "account": {"points": 1, "games": 1, "wins": 0, "tier": 0, "floor": 0, "next": 500, "since": 1},
        "fighters": [
        {"fighter": 3, "points": 1, "games": 1, "wins": 0, "tier": 0, "floor": 0, "next": 200, "since": 1}]});
    ignores_extra::<OwnLevels>(&counted, "");
    ignores_extra::<OwnLevels>(&counted, "/account");
    ignores_extra::<OwnLevels>(&counted, "/fighters/0");
    assert!(serde_json::from_value::<OwnLevels>(json!({"points": "top"})).is_err());
    assert!(serde_json::from_value::<OwnLevels>(json!({"fighters": []})).is_err());
    // A member that is missing is refused, not defaulted.
    assert!(serde_json::from_value::<OwnLevels>(json!({"points": "counted"})).is_err());
    assert!(
        serde_json::from_value::<OwnLevels>(json!({"points": "counted", "fighters": []}))
            .is_err()
    );
    assert!(
        serde_json::from_value::<OwnFighter>(
            json!({"fighter": 3, "games": 1, "wins": 0, "since": 1})
        )
        .is_err()
    );
}

#[test]
fn a_fighter_is_checked_against_its_own_counts() {
    let good = own(3, 1000, 900, 3, 1500, Some(2500));
    assert!(good.check().is_ok());
    // The first tier and the last.
    assert!(own(0, 1, 0, 0, 0, Some(200)).check().is_ok());
    assert!(own(43, 3000, 1000, 4, 2500, None).check().is_ok());
    // One point under the next tier.
    assert!(own(3, 100, 99, 0, 0, Some(200)).check().is_ok());

    // A game is a point and a win at most one more (the class bonus): ten
    // games with five wins are 10 to 15 points, and nothing else.
    let with_points = |points: u64| OwnFighter {
        points,
        ..own(3, 10, 5, 0, 0, Some(200))
    };
    for points in 10..=15 {
        assert!(with_points(points).check().is_ok(), "{points}");
    }
    let refused = [
        ("fighter", own(44, 1, 0, 0, 0, Some(200))),
        ("games", own(3, 0, 0, 0, 0, Some(200))),
        ("wins", own(3, 4, 5, 0, 0, Some(200))),
        ("points", with_points(9)),
        ("points", with_points(16)),
        ("tier", own(3, 1, 0, 5, 0, None)),
        ("next", own(3, 100, 100, 1, 200, Some(200))),
        ("next", own(3, 101, 100, 1, 200, Some(100))),
        // A tier below the top has a next; the top has none.
        ("next", own(3, 3, 2, 0, 0, None)),
        ("next", own(3, 3000, 1000, 4, 2500, Some(5000))),
        // The tier begins at or below the points, and tier 0 begins at 0.
        ("floor", own(3, 100, 99, 1, 200, Some(700))),
        ("floor", own(3, 100, 99, 0, 5, Some(200))),
    ];
    for (field, fighter) in refused {
        assert_eq!(
            fighter.check(),
            Err(Error::InvalidField(field)),
            "{fighter:?}"
        );
    }
    // Counts near the end of the range do not overflow: the sum saturates
    // at `u64::MAX`, which the points then cannot pass.
    let mut huge = own(3, u64::MAX, u64::MAX, 4, 2500, None);
    huge.points = u64::MAX;
    assert!(huge.check().is_ok());
}

#[test]
fn an_account_is_checked_against_its_own_counts_and_frame() {
    assert!(account(0, 0, 0, 0, Some(500)).check().is_ok());
    assert!(account(9, 4, 0, 0, Some(500)).check().is_ok());
    assert!(account(2000, 500, 2, 2000, Some(5000)).check().is_ok());
    assert!(account(9000, 2000, 4, 10_000, None).check().is_ok());

    // Ten games with five wins are 10 to 15 points, as for a fighter.
    let with_points = |points: u64| OwnAccount {
        points,
        ..account(10, 5, 0, 0, Some(500))
    };
    for points in 10..=15 {
        assert!(with_points(points).check().is_ok(), "{points}");
    }
    let mut no_since = account(1, 0, 0, 0, Some(500));
    no_since.since = None;
    let mut early_since = account(0, 0, 0, 0, Some(500));
    early_since.since = Some(1);
    let refused = [
        ("wins", account(4, 5, 0, 0, Some(500))),
        ("points", with_points(9)),
        ("points", with_points(16)),
        ("since", no_since),
        ("since", early_since),
        ("tier", account(1, 0, 5, 0, None)),
        ("next", account(100, 100, 1, 200, Some(200))),
        ("next", account(3, 2, 0, 0, None)),
        ("next", account(9000, 2000, 4, 10_000, Some(20_000))),
        ("floor", account(300, 300, 1, 1000, Some(2000))),
        ("floor", account(100, 99, 0, 5, Some(500))),
    ];
    for (field, account) in refused {
        assert_eq!(account.check(), Err(Error::InvalidField(field)), "{account:?}");
    }
    // The account is checked with the answer, and a bad one fails it.
    let bad = OwnLevels::Counted {
        account: account(4, 5, 0, 0, Some(500)),
        fighters: vec![],
        rival: None,
    };
    assert_eq!(bad.check(), Err(Error::InvalidField("wins")));
}

#[test]
fn own_fighters_are_ascending_unique_and_at_most_44() {
    let at = |fighter: u8| own(fighter, 1, 0, 0, 0, Some(200));
    let counted = |ids: &[u8]| OwnLevels::Counted {
        account: account(1, 0, 0, 0, Some(500)),
        fighters: ids.iter().copied().map(at).collect(),
        rival: None,
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

fn rival_json() -> Value {
    json!({"card": "c1.aZ09-_", "name": "Player 7", "country": "US",
           "badge": {"band": 8, "settled": true}, "won": 41, "lost": 33})
}

#[test]
fn the_most_played_opponent_rides_beside_the_account_and_is_optional() {
    let rival = RivalCard {
        card: "c1.aZ09-_".into(),
        name: "Player 7".into(),
        country: Some("US".into()),
        badge: Some(SkillBadge::class(8, true)),
        won: 41,
        lost: 33,
    };
    let counted = |rival: Option<RivalCard>| OwnLevels::Counted {
        account: account(9, 4, 0, 0, Some(500)),
        fighters: vec![],
        rival,
    };
    let with = counted(Some(rival.clone()));
    assert!(with.check().is_ok());
    let wire = serde_json::to_value(&with).unwrap();
    assert_eq!(wire["rival"], rival_json());
    assert_eq!(read::<OwnLevels>(&wire), with);
    // Absent is not written, and reads as none.
    let without = serde_json::to_value(counted(None)).unwrap();
    assert!(without.get("rival").is_none());
    assert_eq!(read::<OwnLevels>(&without), counted(None));
    // Nothing but the card token names the opponent: no Discord ID or
    // Ember ID member exists to carry one.
    let mut members: Vec<String> = wire["rival"].as_object().unwrap().keys().cloned().collect();
    members.sort();
    assert_eq!(members, ["badge", "card", "country", "lost", "name", "won"]);
    // A country and a badge are optional.
    let bare: RivalCard = read(&json!({"card": "c1.a", "name": "Player 7", "won": 1, "lost": 0}));
    assert_eq!((bare.country.clone(), bare.badge.clone()), (None, None));
    assert!(bare.check().is_ok());
    let wire = serde_json::to_value(&bare).unwrap();
    assert!(wire.get("country").is_none() && wire.get("badge").is_none());
}

#[test]
fn an_older_reader_ignores_the_most_played_opponent() {
    // The answer a bridge with the member writes, read by a type from
    // before it: the member is one more unknown one.
    #[derive(Debug, PartialEq, Deserialize)]
    #[serde(tag = "points", rename_all = "snake_case")]
    enum Before {
        Counted {
            account: OwnAccount,
            fighters: Vec<OwnFighter>,
        },
    }
    let mut wire = json!({"points": "counted",
        "account": {"points": 13, "games": 9, "wins": 4, "tier": 0, "floor": 0,
                    "next": 500, "since": 1729468800},
        "fighters": []});
    let before: Before = read(&wire);
    wire["rival"] = rival_json();
    assert_eq!(read::<Before>(&wire), before);
    // Members this type does not know, on the answer and on the rival.
    ignores_extra::<OwnLevels>(&wire, "");
    ignores_extra::<OwnLevels>(&wire, "/rival");
    let OwnLevels::Counted { rival, .. } = read::<OwnLevels>(&wire) else {
        panic!("counted");
    };
    assert_eq!(rival.unwrap().won, 41);
}

#[test]
fn a_most_played_opponent_is_checked() {
    let good: RivalCard = read(&rival_json());
    let counted = |rival: RivalCard| OwnLevels::Counted {
        account: account(9, 4, 0, 0, Some(500)),
        fighters: vec![],
        rival: Some(rival),
    };
    assert!(counted(good.clone()).check().is_ok());
    let refused = [
        ("card", RivalCard { card: "c2.abc".into(), ..good.clone() }),
        ("card", RivalCard { card: "c1.".into(), ..good.clone() }),
        ("name", RivalCard { name: String::new(), ..good.clone() }),
        ("name", RivalCard { name: "Player\n7".into(), ..good.clone() }),
        ("country", RivalCard { country: Some("usa".into()), ..good.clone() }),
        ("won", RivalCard { won: 0, lost: 0, ..good.clone() }),
        ("badge", RivalCard { badge: Some(SkillBadge::class(0, true)), ..good.clone() }),
    ];
    for (field, rival) in refused {
        assert_eq!(
            counted(rival.clone()).check(),
            Err(Error::InvalidField(field)),
            "{rival:?}"
        );
    }
    // Lost games alone, or won games alone, are a record.
    assert!(RivalCard { won: 0, lost: 5, ..good.clone() }.check().is_ok());
    assert!(RivalCard { won: 5, lost: 0, ..good }.check().is_ok());
}

#[test]
fn class_bonus_points_are_asked_at_the_points_path_with_its_query() {
    assert_eq!(
        CLASS_BONUS_POINTS_PATH,
        format!("{POINTS_PATH}?{CLASS_BONUS_QUERY}")
    );
    assert_ne!(CLASS_BONUS_FEATURE, POINTS_FEATURE);
}
