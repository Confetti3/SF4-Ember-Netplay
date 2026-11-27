//! A player's own rating, told to that player alone
//! (docs/design/SITE_PLAYER_CARDS.md, "Rating details"): the three numbers
//! behind their class, where the class moves, and what a next game would move,
//! worked out by the rules that rate a game (`weights`, `Rating::after`) and
//! never by a copy of them. The rules' own numbers go with it (`RULES`), so
//! the page that explains them cannot fall behind a retune.
//!
//! Nothing here is ever served of another player: a card, a board and a
//! ticket carry the class alone (`SKILL_BADGE.md`).
use ember_protocol::skill::SKILL_BANDS;
use rusqlite::{OptionalExtension, Transaction};
use serde::Serialize;

use super::{
    BAND_FLOORS, BAND_MARGIN, CLEAR_DEVIATIONS, FULL_REPEATS, FULL_WEIGHT_GAP, MIN_WEIGHT,
    MIN_WEIGHT_GAP, REPEAT_WEIGHT, SECS_PER_PERIOD, SETTLED_DEVIATION, SITTING_SECS,
    UNSURE_DEVIATION, UPSET_WEIGHT, band_of, glicko::Rating, rating_at, shown_band, takes_part,
    weights,
};
use crate::error::Result;

/// The numbers the rating rules are made of, as they stand in this bridge.
#[derive(Serialize)]
pub struct Rules {
    start_rating: f64,
    start_deviation: f64,
    start_volatility: f64,
    /// The lowest rating of each class above the first, D+ to Grand Master.
    floors: [f64; SKILL_BANDS as usize - 1],
    /// How far past a class's edge a rating goes before the class follows.
    margin: f64,
    /// The deviation at or below which a rating stands behind a class.
    settled_deviation: f64,
    /// The deviation above which every game counts in full.
    unsure_deviation: f64,
    /// The deviations a loss to a lower-rated player must be clear of.
    clear_deviations: f64,
    /// Up to this many rating points apart a game counts in full.
    full_gap: f64,
    /// From this many apart it counts `min_weight`.
    min_gap: f64,
    min_weight: f64,
    /// The least a lower-rated winner's game counts.
    upset_weight: f64,
    /// Games against one opponent in a sitting that count in full.
    full_repeats: u32,
    /// What each game past them counts.
    repeat_weight: f64,
    /// A sitting ends after this long without a game between the two.
    sitting_secs: u64,
}

pub const RULES: Rules = Rules {
    start_rating: Rating::NEW.rating,
    start_deviation: Rating::NEW.deviation,
    start_volatility: Rating::NEW.volatility,
    floors: BAND_FLOORS,
    margin: BAND_MARGIN,
    settled_deviation: SETTLED_DEVIATION,
    unsure_deviation: UNSURE_DEVIATION,
    clear_deviations: CLEAR_DEVIATIONS,
    full_gap: FULL_WEIGHT_GAP,
    min_gap: MIN_WEIGHT_GAP,
    min_weight: MIN_WEIGHT,
    upset_weight: UPSET_WEIGHT,
    full_repeats: FULL_REPEATS,
    repeat_weight: REPEAT_WEIGHT,
    sitting_secs: SITTING_SECS,
};

/// A player's rating as it stands now, for that player.
#[derive(Serialize)]
pub struct OwnRating {
    /// False while the account does not take part in ranking: the rating is
    /// kept as it was and games move nothing.
    takes_part: bool,
    rating: f64,
    /// Grown by the days since the last game.
    deviation: f64,
    volatility: f64,
    /// The class the next room ticket shows, 1 to `SKILL_BANDS`; 0 before one.
    band: u8,
    /// Whether the rating stands behind the class; a class that is not is
    /// shown faint.
    settled: bool,
    /// The rating at which the class moves up; none at the top and before a
    /// class.
    #[serde(skip_serializing_if = "Option::is_none")]
    up_at: Option<f64>,
    /// The rating under which the class moves down; none at the bottom and
    /// before a class.
    #[serde(skip_serializing_if = "Option::is_none")]
    down_below: Option<f64>,
    /// Whole days without a game until the class shows faint; none while it
    /// is not settled.
    #[serde(skip_serializing_if = "Option::is_none")]
    faint_in_days: Option<u64>,
    /// The last rated game, unix seconds.
    played_at: u64,
    examples: Vec<Example>,
}

/// What a next game against one kind of opponent would move.
#[derive(Serialize)]
pub struct Example {
    /// The opponent's rating.
    opponent: f64,
    /// The class of that rating; 0 for a new player, who has none yet.
    band: u8,
    win: Outcome,
    loss: Outcome,
}

/// One result of an example game.
#[derive(Serialize)]
pub struct Outcome {
    /// How much of a game it counts as, above 0 and at most 1.
    weight: f64,
    /// What it adds to the player's rating; negative for a loss.
    change: f64,
}

/// How far from the player's own rating the examples' settled opponents are:
/// a peer, one above the gap that counts in full and one far below it.
const EXAMPLE_GAPS: [f64; 3] = [0.0, 200.0, -400.0];
/// A settled opponent's deviation in the examples: about fifty games in.
const EXAMPLE_DEVIATION: f64 = 70.0;

/// The rating of Discord account `account` at `now`, for its own player.
/// None before its first rated game.
pub fn own_rating(tx: &Transaction<'_>, account: &str, now: u64) -> Result<Option<OwnRating>> {
    let stored = tx
        .query_row(
            "SELECT rating, deviation, volatility, band, played_at
             FROM skill_ratings WHERE game = 'usf4' AND discord_user_id = ?1",
            [account],
            |row| Ok((rating_at(row, 0, now)?, row.get::<_, u64>(4)?)),
        )
        .optional()?;
    let Some(((rating, shown), played_at)) = stored else {
        return Ok(None);
    };
    Ok(Some(explain(
        rating,
        shown,
        played_at,
        takes_part(tx, account)?,
    )))
}

/// `rating`, aged to now, with the band `shown` last and its last game.
fn explain(rating: Rating, shown: u8, played_at: u64, takes_part: bool) -> OwnRating {
    let settled = rating.deviation <= SETTLED_DEVIATION;
    let band = if settled {
        shown_band(shown, rating.rating)
    } else {
        shown
    };
    let (up_at, down_below) = class_edges(band);
    let settled_opponents = EXAMPLE_GAPS.map(|gap| Rating {
        rating: rating.rating + gap,
        deviation: EXAMPLE_DEVIATION,
        volatility: Rating::NEW.volatility,
    });
    let examples = settled_opponents
        .into_iter()
        .map(|opponent| (opponent, band_of(opponent.rating)))
        .chain([(Rating::NEW, 0)])
        .map(|(opponent, band)| Example {
            opponent: tenth(opponent.rating),
            band,
            win: outcome(rating, opponent, true),
            loss: outcome(rating, opponent, false),
        })
        .collect();
    OwnRating {
        takes_part,
        rating: tenth(rating.rating),
        deviation: tenth(rating.deviation),
        volatility: (rating.volatility * 10_000.0).round() / 10_000.0,
        band,
        settled: band != 0 && settled,
        up_at,
        down_below,
        // The first whole day on which the deviation is past the settled one.
        faint_in_days: rating
            .idle_until(SETTLED_DEVIATION)
            .filter(|_| band != 0)
            .map(|periods| (periods * SECS_PER_PERIOD as f64 / 86_400.0).floor() as u64 + 1),
        played_at,
        examples,
    }
}

/// Where the class shown as `band` moves (`shown_band`): up once the rating
/// is the margin past the next class's floor, down once it is the margin
/// under its own.
fn class_edges(band: u8) -> (Option<f64>, Option<f64>) {
    // The floor of class `band`, which only the classes above the first have.
    let floor_of = |band: usize| band.checked_sub(2).and_then(|at| BAND_FLOORS.get(at));
    let band = usize::from(band);
    if band == 0 {
        return (None, None);
    }
    (
        floor_of(band + 1).map(|floor| floor + BAND_MARGIN),
        floor_of(band).map(|floor| floor - BAND_MARGIN),
    )
}

/// What a game `own` wins or loses against `opponent` counts and moves.
fn outcome(own: Rating, opponent: Rating, won: bool) -> Outcome {
    let (weight, score) = if won {
        (weights(own, opponent).0, 1.0)
    } else {
        (weights(opponent, own).1, 0.0)
    };
    Outcome {
        weight: (weight * 100.0).round() / 100.0,
        change: tenth(own.after(opponent, score, weight).rating - own.rating),
    }
}

fn tenth(value: f64) -> f64 {
    (value * 10.0).round() / 10.0
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rating(rating: f64, deviation: f64) -> Rating {
        Rating {
            rating,
            deviation,
            volatility: 0.0594,
        }
    }

    #[test]
    fn a_class_moves_a_margin_past_its_edges() {
        // Class A is band 7, from 1700 to 1799.
        let own = explain(rating(1742.0, 68.0), 7, 0, true);
        assert_eq!((own.band, own.settled), (7, true));
        assert_eq!((own.up_at, own.down_below), (Some(1825.0), Some(1675.0)));
        // The edges are where `shown_band` itself moves the class.
        assert_eq!(shown_band(7, 1824.9), 7);
        assert_eq!(shown_band(7, 1825.0), 8);
        assert_eq!(shown_band(7, 1675.0), 7);
        assert_eq!(shown_band(7, 1674.9), 6);
        // The lowest class has nowhere to drop to, the highest nowhere to rise.
        assert_eq!(class_edges(1), (Some(1225.0), None));
        assert_eq!(class_edges(SKILL_BANDS), (None, Some(2025.0)));
        assert_eq!(class_edges(0), (None, None));
        assert_eq!(class_edges(u8::MAX), (None, None));
    }

    #[test]
    fn a_placing_rating_has_no_class_and_no_edges() {
        let own = explain(rating(1560.0, 240.0), 0, 0, true);
        assert_eq!((own.band, own.settled), (0, false));
        assert_eq!((own.up_at, own.down_below), (None, None));
        assert_eq!(own.faint_in_days, None);
    }

    #[test]
    fn a_class_shows_faint_after_most_of_a_year_without_a_game() {
        let own = explain(rating(1742.0, 68.0), 7, 0, true);
        let days = own.faint_in_days.unwrap();
        assert!((300..=365).contains(&days), "{days}");
        // The day before, the rating still stands behind the class; that day it does not.
        let before = rating(1742.0, 68.0).idle((days - 1) as f64);
        let then = rating(1742.0, 68.0).idle(days as f64);
        assert!(before.deviation <= SETTLED_DEVIATION && then.deviation > SETTLED_DEVIATION);
        // A class that already shows faint has no such day.
        assert_eq!(
            explain(rating(1742.0, 230.0), 7, 0, true).faint_in_days,
            None
        );
    }

    #[test]
    fn the_examples_are_the_games_the_bridge_would_rate() {
        let own = rating(1742.0, 68.0);
        let told = explain(own, 7, 0, true);
        let [peer, above, below, new] = &told.examples[..] else {
            panic!("four examples");
        };
        // A peer counts in full, and losing costs about what winning earns.
        assert_eq!(
            (peer.band, peer.win.weight, peer.loss.weight),
            (7, 1.0, 1.0)
        );
        assert!(peer.win.change > 10.0 && peer.loss.change < -10.0);
        // 200 above: past the full gap, so both count less, and the loss is light.
        assert_eq!(above.opponent, 1942.0);
        assert!(above.win.weight < 1.0 && above.win.weight == above.loss.weight);
        assert!(above.win.change > -above.loss.change);
        // 400 below: a win is worth next to nothing and a loss stays light.
        assert!(below.win.weight < 0.3 && below.win.change < 1.5);
        assert!(below.loss.change < 0.0 && below.loss.change > -10.0);
        // A new player's games count in full, whatever the gap.
        assert_eq!((new.band, new.opponent), (0, 1500.0));
        assert_eq!((new.win.weight, new.loss.weight), (1.0, 1.0));
        // Each is exactly what rating that game would do.
        let opponent = Rating {
            rating: 1942.0,
            deviation: EXAMPLE_DEVIATION,
            volatility: Rating::NEW.volatility,
        };
        let (winner_weight, _) = weights(own, opponent);
        let won = own.after(opponent, 1.0, winner_weight).rating - own.rating;
        assert_eq!(above.win.change, tenth(won));
    }

    #[test]
    fn the_rules_told_are_the_rules_applied() {
        assert_eq!(RULES.floors[3], Rating::NEW.rating);
        assert_eq!(band_of(RULES.floors[8]), SKILL_BANDS);
        assert_eq!(RULES.full_repeats, FULL_REPEATS);
    }
}
