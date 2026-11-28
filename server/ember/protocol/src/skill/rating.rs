//! A player's own rating, told to that player alone (docs/design/SITE_PLAYER_CARDS.md,
//! "Rating details"): the three numbers behind their class, where the class moves,
//! and what a next game would move, beside the rules the bridge rates by (`Rules`).
//!
//! `GET` [`RATING_PATH`] answers this for the calling player: their rating is
//! absent before their first rated game and while the bridge keeps classes
//! hidden, and the rules it follows either way. The numbers are never served
//! of another player.
use serde::{Deserialize, Serialize};

use crate::{
    Error, Result,
    skill::SKILL_BANDS,
};

/// `GET /v1/ranking/rating`: the caller's own rating details.
pub const RATING_PATH: &str = "/v1/ranking/rating";

/// The most examples an answer lists.
const MAX_EXAMPLES: usize = 8;

/// The numbers the rating rules are made of, as they stand in this bridge.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Rules {
    pub start_rating: f64,
    pub start_deviation: f64,
    pub start_volatility: f64,
    /// The lowest rating of each class above the first, D+ to Grand Master.
    pub floors: [f64; SKILL_BANDS as usize - 1],
    /// How far past a class's edge a rating goes before the class follows.
    pub margin: f64,
    /// The deviation at or below which a rating stands behind a class.
    pub settled_deviation: f64,
    /// The deviation above which every game counts in full.
    pub unsure_deviation: f64,
    /// The deviations a loss to a lower-rated player must be clear of.
    pub clear_deviations: f64,
    /// Up to this many rating points apart a game counts in full.
    pub full_gap: f64,
    /// From this many apart it counts `min_weight`.
    pub min_gap: f64,
    pub min_weight: f64,
    /// The least a lower-rated winner's game counts.
    pub upset_weight: f64,
    /// Games against one opponent in a sitting that count in full.
    pub full_repeats: u32,
    /// What each game past them counts.
    pub repeat_weight: f64,
    /// A sitting ends after this long without a game between the two.
    pub sitting_secs: u64,
}

/// A player's rating as it stands now, for that player.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct OwnRating {
    /// False while the account does not take part in ranking: the rating is
    /// kept as it was and games move nothing.
    pub takes_part: bool,
    pub rating: f64,
    /// Grown by the days since the last game.
    pub deviation: f64,
    pub volatility: f64,
    /// The class the next room ticket shows, 1 to `SKILL_BANDS`; 0 before one.
    pub band: u8,
    /// Whether the rating stands behind the class; a class that is not is
    /// shown faint.
    pub settled: bool,
    /// The rating at which the class moves up; none at the top and before a
    /// class.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub up_at: Option<f64>,
    /// The rating under which the class moves down; none at the bottom and
    /// before a class.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub down_below: Option<f64>,
    /// Whole days without a game until the class shows faint; none while it
    /// is not settled.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub faint_in_days: Option<u64>,
    /// The last rated game, unix seconds.
    pub played_at: u64,
    pub examples: Vec<Example>,
}

/// What a next game against one kind of opponent would move.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Example {
    /// The opponent's rating.
    pub opponent: f64,
    /// The class of that rating; 0 for a new player, who has none yet.
    pub band: u8,
    pub win: Outcome,
    pub loss: Outcome,
}

/// One result of an example game.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Outcome {
    /// How much of a game it counts as, above 0 and at most 1.
    pub weight: f64,
    /// What it adds to the player's rating; negative for a loss.
    pub change: f64,
}

/// What `GET /v1/ranking/rating` answers: the caller's rating (absent before a
/// rated game and while the bridge hides classes) and the rules it follows.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct RatingDetails {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rating: Option<OwnRating>,
    pub rules: Rules,
}

/// Every number of a rating and its examples is a real one: no NaN, no infinity.
fn rating_is_finite(rating: &OwnRating) -> bool {
    let scalars = [rating.rating, rating.deviation, rating.volatility];
    let edge = |value: Option<f64>| value.map_or(true, |edge| edge.is_finite());
    scalars.iter().all(|value| value.is_finite())
        && edge(rating.up_at)
        && edge(rating.down_below)
        && rating.examples.iter().all(|example| {
            example.opponent.is_finite()
                && example.win.change.is_finite()
                && example.loss.change.is_finite()
        })
}

impl RatingDetails {
    /// Every number finite, the class within `SKILL_BANDS`, at most
    /// [`MAX_EXAMPLES`] examples and each of their outcomes counting above
    /// nothing and no more than a whole game.
    pub fn check(&self) -> Result<()> {
        let rules = &self.rules;
        let numbers = [
            rules.start_rating,
            rules.start_deviation,
            rules.start_volatility,
            rules.margin,
            rules.settled_deviation,
            rules.unsure_deviation,
            rules.clear_deviations,
            rules.full_gap,
            rules.min_gap,
            rules.min_weight,
            rules.upset_weight,
            rules.repeat_weight,
        ];
        if !numbers.iter().all(|value| value.is_finite())
            || !rules.floors.iter().all(|value| value.is_finite())
        {
            return Err(Error::InvalidField("rules"));
        }
        if let Some(rating) = &self.rating {
            if !rating_is_finite(rating) {
                return Err(Error::InvalidField("rating"));
            }
            if rating.band > SKILL_BANDS {
                return Err(Error::InvalidField("band"));
            }
            if rating.examples.len() > MAX_EXAMPLES {
                return Err(Error::InvalidField("examples"));
            }
            for example in &rating.examples {
                let weights = [example.win.weight, example.loss.weight];
                if !weights
                    .iter()
                    .all(|weight| 0.0 < *weight && *weight <= 1.0)
                {
                    return Err(Error::InvalidField("weight"));
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rules() -> Rules {
        Rules {
            start_rating: 1500.0,
            start_deviation: 350.0,
            start_volatility: 0.06,
            floors: [1050.0, 1200.0, 1350.0, 1500.0, 1650.0, 1800.0, 1950.0, 2100.0, 2250.0],
            margin: 25.0,
            settled_deviation: 100.0,
            unsure_deviation: 200.0,
            clear_deviations: 2.0,
            full_gap: 200.0,
            min_gap: 600.0,
            min_weight: 0.25,
            upset_weight: 0.1,
            full_repeats: 3,
            repeat_weight: 0.05,
            sitting_secs: 1800,
        }
    }

    fn own() -> OwnRating {
        OwnRating {
            takes_part: true,
            rating: 1742.0,
            deviation: 68.0,
            volatility: 0.0594,
            band: 7,
            settled: true,
            up_at: Some(1825.0),
            down_below: Some(1675.0),
            faint_in_days: Some(302),
            played_at: 1_700_000_000,
            examples: [
                Example {
                    opponent: 1742.0,
                    band: 7,
                    win: Outcome {
                        weight: 1.0,
                        change: 36.5,
                    },
                    loss: Outcome {
                        weight: 1.0,
                        change: -36.5,
                    },
                },
            ]
            .into_iter()
            .collect(),
        }
    }

    #[test]
    fn the_details_read_back() {
        let told = RatingDetails {
            rating: Some(own()),
            rules: rules(),
        };
        assert!(told.check().is_ok(), "{told:?}");
        let written = serde_json::to_string(&told).unwrap();
        assert_eq!(
            serde_json::from_str::<RatingDetails>(&written).unwrap(),
            told,
            "{written}"
        );
        // With no rating the answer leaves it out, and reads back with none.
        let empty = RatingDetails {
            rating: None,
            rules: rules(),
        };
        let written = serde_json::to_string(&empty).unwrap();
        assert!(!written.contains("\"rating\""), "{written}");
        assert_eq!(
            serde_json::from_str::<RatingDetails>(&written).unwrap(),
            empty,
            "{written}"
        );
    }

    #[test]
    fn a_nan_rating_is_refused() {
        let mut nan = own();
        nan.rating = f64::NAN;
        assert_eq!(
            RatingDetails {
                rating: Some(nan),
                rules: rules(),
            }
            .check(),
            Err(Error::InvalidField("rating")),
        );
    }
}
