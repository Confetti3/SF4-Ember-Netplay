//! What a fighter's game saw of one game besides its result: both fighters'
//! characters and each round (docs/design/BRACKET_SITE.md, "Live stats").
//! A report of version 2 carries them; the bridge keeps a game's facts when
//! both fighters' first reports say the same, and shows them with the game.
use serde::{Deserialize, Serialize};

use crate::{Error, Result, encoding::Counter};

/// In a bridge's capabilities when it takes reports of version 2.
pub const FEATURE: &str = "reports.facts";
/// Rounds in one game: the longest a game can run, with drawn rounds.
pub const MAX_ROUNDS: usize = 9;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct GameFacts {
    /// P1's fighter, then P2's.
    pub fighters: [Fighter; 2],
    /// Each round, in the order played.
    pub rounds: Vec<Round>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Fighter {
    /// The game's own three-letter code (`RYU`, `BLR`), upper case.
    pub character: String,
    /// The costume (0 is the first) and colour (0 is the first) picked.
    pub costume: u8,
    pub color: u8,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Round {
    /// 0 for P1, 1 for P2; none for a drawn round.
    pub winner: Option<u8>,
    pub finish: Finish,
    /// The winner lost no health in it.
    pub perfect: bool,
    /// The game frames the round ran from and to, as the replay counts them.
    pub from_frame: Counter,
    pub to_frame: Counter,
}

/// How a round ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Finish {
    /// A knockout by a normal, special or throw.
    Ko,
    /// A knockout by a Super combo, or an Ultra combo.
    Super,
    Ultra,
    /// Time ran out.
    Time,
    /// Both fighters knocked out at once.
    DoubleKo,
}

impl GameFacts {
    pub fn check(&self) -> Result<()> {
        let fighter_ok = |fighter: &Fighter| {
            fighter.character.len() == 3
                && fighter.character.bytes().all(|b| b.is_ascii_uppercase())
        };
        if !self.fighters.iter().all(fighter_ok) {
            return Err(Error::InvalidField("facts.fighters"));
        }
        if self.rounds.is_empty() || self.rounds.len() > MAX_ROUNDS {
            return Err(Error::InvalidField("facts.rounds"));
        }
        let mut last = 0;
        for round in &self.rounds {
            let drawn =
                matches!(round.finish, Finish::DoubleKo | Finish::Time) && round.winner.is_none();
            let valid = round.winner.map_or(drawn, |winner| {
                winner <= 1 && round.finish != Finish::DoubleKo
            }) && !(round.perfect && round.winner.is_none())
                && round.from_frame.0 >= last
                && round.to_frame.0 > round.from_frame.0;
            if !valid {
                return Err(Error::InvalidField("facts.rounds"));
            }
            last = round.to_frame.0;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    fn facts(rounds: serde_json::Value) -> Result<GameFacts> {
        let facts: GameFacts = serde_json::from_value(json!({
            "fighters": [{ "character": "RYU", "costume": 0, "color": 2 }, { "character": "BLR", "costume": 1, "color": 0 }],
            "rounds": rounds,
        }))
        .map_err(|_| Error::InvalidField("facts"))?;
        facts.check().map(|()| facts)
    }

    fn round(
        winner: Option<u8>,
        finish: &str,
        perfect: bool,
        from: u64,
        to: u64,
    ) -> serde_json::Value {
        json!({ "winner": winner, "finish": finish, "perfect": perfect, "from_frame": from.to_string(), "to_frame": to.to_string() })
    }

    #[test]
    fn rounds_follow_each_other_and_say_how_they_ended() {
        let game = facts(json!([
            round(Some(0), "ko", true, 100, 2000),
            round(None, "double_ko", false, 2100, 3000),
            round(Some(1), "ultra", false, 3100, 5000),
            round(None, "time", false, 5100, 9000),
            round(Some(0), "time", false, 9100, 12000),
        ]))
        .unwrap();
        assert_eq!(game.rounds[2].finish, Finish::Ultra);
        for bad in [
            json!([]),
            json!([round(Some(2), "ko", false, 1, 2)]),
            json!([round(Some(0), "double_ko", false, 1, 2)]),
            json!([round(None, "ko", false, 1, 2)]),
            json!([round(None, "time", true, 1, 2)]),
            json!([round(Some(0), "ko", false, 5, 5)]),
            json!([
                round(Some(0), "ko", false, 1, 9),
                round(Some(0), "ko", false, 5, 20)
            ]),
            json!([round(Some(0), "flawless", false, 1, 2)]),
        ] {
            assert!(facts(bad.clone()).is_err(), "{bad}");
        }
        assert!(
            facts(json!(vec![
                round(Some(0), "ko", false, 1, 2);
                MAX_ROUNDS + 1
            ]))
            .is_err()
        );
    }
}
