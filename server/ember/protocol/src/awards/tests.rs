use serde_json::json;

use super::*;
use crate::{
    cards::PlayerCard,
    levels::{
        OwnAccount, OwnLevels,
        tests::{ignores_extra, read},
    },
};

#[test]
fn a_family_id_is_a_short_lower_case_word_with_hyphens() {
    for good in ["rank", "matches", "a", "opponents", "a-b-9", &"x".repeat(MAX_FAMILY_ID)] {
        assert!(is_family_id(good), "{good:?}");
    }
    for bad in ["", "Rank", "a b", "a_b", "é", "a\n", &"x".repeat(MAX_FAMILY_ID + 1)] {
        assert!(!is_family_id(bad), "{bad:?}");
    }
}

#[test]
fn a_mark_is_the_largest_milestone_a_count_has_reached() {
    for (count, rival, opponents) in [
        (0, 0, 0),
        (9, 0, 0),
        (10, 10, 10),
        (49, 10, 10),
        (50, 50, 50),
        (100, 100, 50),
        (199, 100, 50),
        (200, 100, 200),
        (499, 250, 200),
        (500, 250, 500),
        (u64::MAX, 250, 500),
    ] {
        assert_eq!(mark(&RIVAL_MARKS, count), rival, "{count} games");
        assert_eq!(mark(&OPPONENT_MARKS, count), opponents, "{count} opponents");
    }
    for marks in [&RIVAL_MARKS, &OPPONENT_MARKS] {
        assert!(marks.windows(2).all(|pair| pair[0] < pair[1]));
    }
}

/// The needs of the family `family` in the game's catalog, in order.
fn catalog_needs(text: &str, family: &str) -> Vec<u64> {
    let call = format!("SF4E_LEVEL(\"{family}\", ");
    text.match_indices(&call)
        .map(|(at, _)| {
            let rest = &text[at + call.len()..];
            rest[..rest.find(',').unwrap()].trim().parse().unwrap()
        })
        .collect()
}

#[test]
fn the_card_milestones_are_the_catalogs_rival_and_opponent_levels() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../src/netplay/Awards.hxx");
    // A crate built outside the repository has no catalog to read.
    let Ok(text) = std::fs::read_to_string(path) else {
        return;
    };
    assert_eq!(catalog_needs(&text, "rival"), RIVAL_MARKS);
    assert_eq!(catalog_needs(&text, "opponents"), OPPONENT_MARKS);
}

#[test]
fn counts_shown_on_a_card_are_exact_but_for_the_rival_and_opponent_milestones() {
    let own = AwardCounts {
        games: 137,
        wins: 61,
        best_streak: 7,
        peak_class: 6,
        rival: 99,
        opponents: 499,
    };
    assert_eq!(
        own.shown(true),
        AwardCounts {
            rival: 50,
            opponents: 200,
            ..own
        }
    );
    // A player who does not take part in ranking shows no class.
    assert_eq!(own.shown(false).peak_class, 0);
    assert_eq!(own.shown(false).games, 137);
    assert!(AwardCounts::default().shown(true).is_empty());
}

#[test]
fn counts_are_checked_against_each_other() {
    let good = AwardCounts {
        games: 10,
        wins: 4,
        best_streak: 4,
        peak_class: SKILL_BANDS,
        ..AwardCounts::default()
    };
    assert!(good.check().is_ok());
    for (field, bad) in [
        ("wins", AwardCounts { wins: 11, ..good }),
        ("best_streak", AwardCounts { best_streak: 11, ..good }),
        ("peak_class", AwardCounts { peak_class: SKILL_BANDS + 1, ..good }),
    ] {
        assert_eq!(bad.check(), Err(Error::InvalidField(field)));
    }
}

#[test]
fn the_award_counters_are_optional_on_every_answer() {
    // An own answer from a bridge before them has none, and reads as 0.
    let old = json!({"points": "counted",
        "account": {"points": 13, "games": 9, "wins": 4, "tier": 0, "floor": 0,
                    "next": 500, "since": 1729468800},
        "fighters": []});
    let OwnLevels::Counted { account, .. } = read::<OwnLevels>(&old) else {
        panic!("counted");
    };
    assert_eq!(account.awards, AwardCounts { games: 9, wins: 4, ..AwardCounts::default() });
    assert_eq!(serde_json::to_value(&account).unwrap().get("best_streak"), None);

    // A bridge with them writes them in the account, beside the points, only while they are not 0.
    let mut with = account.clone();
    with.awards.best_streak = 7;
    with.awards.peak_class = 6;
    with.awards.rival = 3;
    with.awards.opponents = 2;
    assert!(with.check().is_ok());
    let wire = serde_json::to_value(&with).unwrap();
    assert_eq!(
        [&wire["best_streak"], &wire["peak_class"], &wire["rival"], &wire["opponents"]],
        [&json!(7), &json!(6), &json!(3), &json!(2)]
    );
    assert_eq!(read::<OwnAccount>(&wire), with);
    // A streak longer than the games played, or a class past the top, is no answer.
    let mut long = with.clone();
    long.awards.best_streak = 10;
    assert_eq!(long.check(), Err(Error::InvalidField("best_streak")));
    let mut high = with;
    high.awards.peak_class = SKILL_BANDS + 1;
    assert_eq!(high.check(), Err(Error::InvalidField("peak_class")));
}

#[test]
fn a_card_carries_the_counters_behind_the_awards() {
    let bare = json!({"account": {"points": 2, "tier": 0}, "fighters": [], "relation": "none"});
    let card: PlayerCard = read(&bare);
    assert!(card.awards.is_empty());
    assert_eq!(serde_json::to_value(&card).unwrap(), bare);

    // Games, wins and the best streak are the counts themselves, so a card's
    // record cells show what the player has done.
    let marked = json!({"account": {"points": 2, "tier": 0}, "fighters": [], "relation": "none",
        "awards": {"games": 137, "wins": 61, "best_streak": 7, "peak_class": 6, "rival": 50}});
    let card: PlayerCard = read(&marked);
    assert_eq!(
        card.awards,
        AwardCounts { games: 137, wins: 61, best_streak: 7, peak_class: 6, rival: 50, opponents: 0 }
    );
    assert!(card.check().is_ok());
    assert_eq!(serde_json::to_value(&card).unwrap(), marked);
    // A count of 0 is not written; a member a later bridge adds is ignored.
    let sparse = PlayerCard {
        awards: AwardCounts { games: 1, ..AwardCounts::default() },
        ..card.clone()
    };
    assert_eq!(
        serde_json::to_value(&sparse).unwrap()["awards"],
        json!({"games": 1, "wins": 0})
    );
    ignores_extra::<PlayerCard>(&marked, "/awards");
    // A class past the top fails the card.
    let mut bad = card;
    bad.awards.peak_class = SKILL_BANDS + 1;
    assert_eq!(bad.check(), Err(Error::InvalidField("peak_class")));
}
