//! What a tournament's configuration says about its sets besides who plays
//! them: how long each is (`Rounds`) and which wait (`Hold`).
use std::collections::HashSet;

use ember_protocol::matches::games_to_win_valid;
use serde::{Deserialize, Serialize};

use crate::site::{Phase, Set};

/// Set lengths by round. The site does not say how long a set is.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Rounds {
    /// First to this many games, unless a rule says otherwise.
    pub games_to_win: u8,
    /// Read in order: the first rule that covers a set sets its length, so
    /// a narrower rule goes before a wider one.
    #[serde(default)]
    pub rules: Vec<RoundRule>,
    /// The sets played among the last `top` players, when no rule covers
    /// them (`Shape::in_top`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub top: Option<Top>,
    /// The grand final's length (and its reset's), over every other rule; a
    /// single elimination bracket's final counts as its grand final.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub grand_final: Option<u8>,
}

/// First to `games_to_win` from the last `top` players on.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Top {
    pub top: u32,
    pub games_to_win: u8,
}

/// How far a bracket's rounds go, from the sets read: its last winners round
/// and its deepest losers round (both sites number those below zero). With
/// no losers rounds it is single elimination.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Shape {
    pub winners: u32,
    pub losers: u32,
}

impl Shape {
    /// The shape of `phase`'s bracket among `sets`.
    pub fn of<'a>(sets: impl IntoIterator<Item = &'a Set>, phase: Option<&Phase>) -> Self {
        sets.into_iter()
            .filter(|set| set.extras.phase.as_ref() == phase)
            .fold(Self::default(), |shape, set| Self {
                winners: shape.winners.max(u32::try_from(set.round).unwrap_or(0)),
                losers: shape.losers.max(if set.round < 0 {
                    set.round.unsigned_abs()
                } else {
                    0
                }),
            })
    }

    /// The winners rounds before the grand final: a double elimination
    /// bracket of 2^k players has 2k - 2 losers rounds.
    // ponytail: a bracket with byes may have one losers round more or less;
    // the count from the site's own placements if that misplaces a set.
    fn before_final(self) -> u32 {
        if self.losers == 0 {
            self.winners
        } else {
            self.losers / 2 + 1
        }
    }

    /// Whether `round` is the grand final or its reset.
    fn grand_final(self, round: i32) -> bool {
        let Ok(round) = u32::try_from(round) else {
            return false;
        };
        round > 0
            && if self.losers == 0 {
                round == self.winners
            } else {
                round > self.before_final()
            }
    }

    /// How many players a set of `round` is played among: every winners
    /// round halves the field, and a losers round holds half of what is
    /// left there, two rounds at each size.
    fn players(self, round: i32) -> u32 {
        let size = 1u32 << self.before_final().min(16);
        let halvings = if round > 0 {
            round.unsigned_abs() - 1
        } else {
            round.unsigned_abs().div_ceil(2)
        };
        size.checked_shr(halvings).unwrap_or(0).max(1)
    }

    /// Whether a set of `round` is played among the last `top` players: in
    /// single elimination once `top` or fewer play; in double elimination
    /// once the winners side holds the largest power of two up to half of
    /// `top` and the losers side the rest (top 8: 4 and 4; top 6: 2 and 4).
    /// A bracket of `top` players or fewer is all top.
    fn in_top(self, top: u32, round: i32) -> bool {
        if self.players(1) <= top {
            return true;
        }
        if self.losers == 0 {
            return self.players(round) <= top;
        }
        let winners = 1u32 << (top / 2).max(1).ilog2();
        if round > 0 {
            self.players(round) <= winners
        } else {
            self.players(round) <= top.saturating_sub(winners)
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RoundRule {
    /// Only sets of this stage of the event: a start.gg phase's name (as
    /// the event's bracket page shows it, ignoring case) or ID, Challonge's
    /// `group` or `final` stage, or an http service's `phase`. Any stage
    /// when absent. A set that names no stage is covered only by a rule
    /// without one.
    #[serde(default)]
    pub phase: Option<String>,
    /// The first round covered; round 1 when absent.
    #[serde(default = "first_round")]
    pub from_round: u32,
    /// The last round covered; every later round when absent.
    #[serde(default)]
    pub to_round: Option<u32>,
    #[serde(default)]
    pub side: Side,
    pub games_to_win: u8,
}

fn first_round() -> u32 {
    1
}

/// Which rounds a rule counts. Challonge numbers the losers bracket's rounds
/// below zero and every other round above it, so `winners` also covers
/// single elimination, round robin and swiss.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Side {
    Winners,
    Losers,
    #[default]
    Any,
}

/// Sets the connector does not create a match for yet, each kind off unless
/// asked for.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Hold {
    /// A set the site shows assigned to a stream (start.gg): played on the
    /// stream setup, so it waits, logged once, until it is taken off the
    /// stream. A set live in Ember is left as it is, but one whose Ember
    /// match ended without a result is a new attempt, and waits like any
    /// other.
    #[serde(default)]
    pub streamed: bool,
}

impl RoundRule {
    fn covers(&self, set: &Set) -> bool {
        let side = if set.round < 0 {
            Side::Losers
        } else {
            Side::Winners
        };
        let number = set.round.unsigned_abs();
        self.phase.as_deref().is_none_or(|wanted| {
            set.extras
                .phase
                .as_ref()
                .is_some_and(|phase| phase.is(wanted))
        }) && (self.side == Side::Any || self.side == side)
            && number >= self.from_round
            && self.to_round.is_none_or(|to| number <= to)
    }
}

impl Rounds {
    /// How many games `set` is first to: the grand final's length, the first
    /// rule that covers it, the top's, or `games_to_win`. `shape` is its
    /// bracket's, when the reads have shown it.
    pub fn length(&self, set: &Set, shape: Option<Shape>) -> u8 {
        let shape = shape.filter(|shape| shape.winners > 0);
        if let (Some(length), Some(shape)) = (self.grand_final, shape)
            && shape.grand_final(set.round)
        {
            return length;
        }
        if let Some(rule) = self.rules.iter().find(|rule| rule.covers(set)) {
            return rule.games_to_win;
        }
        match (self.top, shape) {
            (Some(top), Some(shape)) if shape.in_top(top.top, set.round) => top.games_to_win,
            _ => self.games_to_win,
        }
    }

    /// Whether a set's length needs its bracket's shape.
    pub fn needs_shape(&self) -> bool {
        self.top.is_some() || self.grand_final.is_some()
    }

    /// Whether any rule is for one phase, so the phase of each set matters.
    pub fn names_phases(&self) -> bool {
        self.rules.iter().any(|rule| rule.phase.is_some())
    }

    pub(crate) fn check(&self) -> Result<(), String> {
        let lengths_valid = std::iter::once(self.games_to_win)
            .chain(self.rules.iter().map(|rule| rule.games_to_win))
            .chain(self.top.map(|top| top.games_to_win))
            .chain(self.grand_final)
            .all(games_to_win_valid);
        if self.top.is_some_and(|top| !(2..=1024).contains(&top.top)) {
            return Err("top is 2 to 1024 players".into());
        }
        if !lengths_valid {
            return Err("games_to_win is 1 to 10".into());
        }
        let ranges_valid = self.rules.iter().all(|rule| {
            rule.from_round >= 1 && rule.to_round.is_none_or(|to| to >= rule.from_round)
        });
        if !ranges_valid {
            return Err("a round rule starts at round 1 or later and ends no earlier".into());
        }
        let phases_valid = self
            .rules
            .iter()
            .filter_map(|rule| rule.phase.as_deref())
            .all(Phase::is_valid_name);
        if !phases_valid {
            return Err(format!(
                "a round rule's phase is 1 to {} characters",
                crate::site::MAX_PHASE
            ));
        }
        Ok(())
    }
}

/// The most phases remembered, so a service that gives each set a phase of
/// its own cannot grow it without end.
const MAX_SEEN: usize = 64;

/// The phases a tournament's reads have shown, so a round rule that names
/// none of them is said once: a mistyped name would otherwise give its sets
/// the default length without a word. A phase may well appear later (top 8
/// starts after pools), so the notice says what has been seen, not that the
/// rule is wrong.
#[derive(Default)]
pub(crate) struct PhaseWatch {
    seen: Vec<Phase>,
    said: HashSet<usize>,
}

impl PhaseWatch {
    /// Takes in a read's sets. Returns what to say now, one line for each
    /// phase rule that matches no phase seen so far and was not said before.
    pub fn read(&mut self, rounds: &Rounds, sets: &[Set]) -> Vec<String> {
        if !rounds.names_phases() || sets.is_empty() {
            return Vec::new();
        }
        for phase in sets.iter().filter_map(|set| set.extras.phase.as_ref()) {
            if self.seen.len() < MAX_SEEN && !self.seen.contains(phase) {
                self.seen.push(phase.clone());
            }
        }
        let mut lines = Vec::new();
        for (index, wanted) in rounds
            .rules
            .iter()
            .enumerate()
            .filter_map(|(index, rule)| Some((index, rule.phase.as_deref()?)))
        {
            if self.seen.iter().any(|phase| phase.is(wanted)) || !self.said.insert(index) {
                continue;
            }
            let seen = if self.seen.is_empty() {
                "none".to_owned()
            } else {
                self.seen
                    .iter()
                    .map(Phase::to_string)
                    .collect::<Vec<_>>()
                    .join(", ")
            };
            lines.push(format!(
                "rule for phase \"{wanted}\" matches no phase; phases seen: {seen}; check its spelling, since sets in a phase it does not name take the next rule or games_to_win"
            ));
        }
        lines
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::site::{Extras, Status};

    fn rule(from: u32, to: Option<u32>, side: Side, games: u8) -> RoundRule {
        RoundRule {
            phase: None,
            from_round: from,
            to_round: to,
            side,
            games_to_win: games,
        }
    }

    fn set(round: i32, phase: Option<Phase>) -> Set {
        Set {
            id: "1".into(),
            round,
            round_label: String::new(),
            entries: None,
            status: Status::Open,
            underway: false,
            winner: None,
            extras: Extras {
                phase,
                ..Extras::default()
            },
        }
    }

    fn phase(name: &str, id: Option<&str>) -> Phase {
        Phase::new(Some(name), id.map(str::to_owned)).unwrap()
    }

    #[test]
    fn the_first_rule_that_covers_a_round_sets_its_length() {
        let rounds = Rounds {
            games_to_win: 2,
            rules: vec![
                rule(5, None, Side::Winners, 4),
                rule(3, None, Side::Any, 3),
                rule(1, Some(2), Side::Losers, 1),
            ],
            top: None,
            grand_final: None,
        };
        let lengths: Vec<u8> = [1, 2, 3, 4, 5, 9, -1, -2, -3, -7]
            .into_iter()
            .map(|round| rounds.length(&set(round, None), None))
            .collect();
        assert_eq!(lengths, [2, 2, 3, 3, 4, 4, 1, 1, 3, 3]);
        // Round 0 is not a losers round.
        assert_eq!(rounds.length(&set(0, None), None), 2);
    }

    #[test]
    fn a_rule_can_name_the_phase_whose_rounds_it_covers() {
        // First to 2 in pools, first to 3 in top 8, first to 5 in its
        // grand final (round 3 of top 8 here). Both phases number rounds
        // from 1, so only the phase tells their round 1s apart.
        let named = |name: &str, games: u8| RoundRule {
            phase: Some(name.into()),
            ..rule(1, None, Side::Any, games)
        };
        let rounds = Rounds {
            games_to_win: 2,
            rules: vec![
                RoundRule {
                    from_round: 3,
                    side: Side::Winners,
                    ..named("Top 8", 5)
                },
                named("top 8", 3),
            ],
            top: None,
            grand_final: None,
        };
        let pools = phase("Pools", Some("1300001"));
        let top = phase("Top 8", Some("1300002"));
        let length =
            |round, phase: Option<&Phase>| rounds.length(&set(round, phase.cloned()), None);
        assert_eq!(length(1, Some(&pools)), 2);
        assert_eq!(length(1, Some(&top)), 3);
        assert_eq!(length(-2, Some(&top)), 3);
        assert_eq!(length(3, Some(&top)), 5);
        // A set with no phase: only rules without one apply.
        assert_eq!(length(1, None), 2);
        // A rule may name the phase by its ID instead.
        let by_id = Rounds {
            games_to_win: 2,
            rules: vec![named("1300002", 3)],
            top: None,
            grand_final: None,
        };
        assert_eq!(by_id.length(&set(1, Some(top.clone())), None), 3);
        assert_eq!(by_id.length(&set(1, Some(pools)), None), 2);
        // A phase known only by its ID is named by it.
        let nameless = Phase::new(None, Some("1300002".into())).unwrap();
        assert_eq!(by_id.length(&set(1, Some(nameless)), None), 3);
    }

    #[test]
    fn a_phase_needs_a_name_or_an_id_and_a_blank_name_is_none() {
        assert_eq!(Phase::new(Some("  "), None), None);
        assert_eq!(Phase::new(None, None), None);
        assert_eq!(
            Phase::new(Some(" Top 8 "), None),
            Some(Phase::named("Top 8"))
        );
        assert!(Phase::is_valid_name(" Top 8 "));
        assert!(!Phase::is_valid_name("  "));
        assert!(Phase::is_valid_name(&"x".repeat(64)));
        assert!(!Phase::is_valid_name(&"x".repeat(65)));
    }

    #[test]
    fn round_rules_are_checked() {
        let with = |rules| Rounds {
            games_to_win: 2,
            rules,
            top: None,
            grand_final: None,
        };
        assert!(with(vec![rule(1, None, Side::Any, 10)]).check().is_ok());
        assert!(with(vec![rule(0, None, Side::Any, 3)]).check().is_err());
        assert!(with(vec![rule(4, Some(3), Side::Any, 3)]).check().is_err());
        assert!(with(vec![rule(1, None, Side::Any, 11)]).check().is_err());
        assert!(
            Rounds {
                games_to_win: 0,
                rules: vec![],
                top: None,
                grand_final: None,
            }
            .check()
            .is_err()
        );
        let phased = |name: &str| {
            with(vec![RoundRule {
                phase: Some(name.into()),
                ..rule(1, None, Side::Any, 3)
            }])
        };
        assert!(phased("Top 8").check().is_ok());
        assert!(phased(" ").check().is_err());
        assert!(phased(&"x".repeat(65)).check().is_err());
    }

    #[test]
    fn a_mistyped_phase_rule_is_said_once_with_the_phases_seen() {
        let rounds = Rounds {
            games_to_win: 2,
            rules: vec![
                RoundRule {
                    phase: Some("Top8".into()),
                    ..rule(1, None, Side::Any, 3)
                },
                RoundRule {
                    phase: Some("pools".into()),
                    ..rule(1, None, Side::Any, 1)
                },
                rule(1, None, Side::Any, 2),
            ],
            top: None,
            grand_final: None,
        };
        let mut watch = PhaseWatch::default();
        // Nothing is said about a read with no sets.
        assert!(watch.read(&rounds, &[]).is_empty());
        let read = [
            set(1, Some(phase("Pools", Some("1")))),
            set(1, Some(phase("Top 8", Some("2")))),
        ];
        let said = watch.read(&rounds, &read);
        assert_eq!(said.len(), 1);
        assert!(
            said[0].starts_with(
                "rule for phase \"Top8\" matches no phase; phases seen: Pools, Top 8;"
            ),
            "{said:?}"
        );
        // Said once, however many reads follow.
        assert!(watch.read(&rounds, &read).is_empty());
        // A rule with no phase is never said, and nor is any when no rule
        // names a phase.
        assert!(
            PhaseWatch::default()
                .read(
                    &Rounds {
                        games_to_win: 2,
                        rules: vec![rule(1, None, Side::Any, 3)],
                        top: None,
                        grand_final: None,
                    },
                    &read
                )
                .is_empty()
        );
        // Sets with no phase at all are said too.
        let said = PhaseWatch::default().read(&rounds, &[set(1, None)]);
        assert!(said[0].contains("phases seen: none;"), "{said:?}");
    }

    /// First to 2, top 8 first to 3, grand final first to 5: the lengths of
    /// every round of a bracket shaped as the reads show it, winners rounds
    /// then losers rounds.
    fn lengths(shape: Shape) -> (Vec<u8>, Vec<u8>) {
        let rounds = Rounds {
            games_to_win: 2,
            rules: vec![],
            top: Some(Top {
                top: 8,
                games_to_win: 3,
            }),
            grand_final: Some(5),
        };
        let all = |sides: std::ops::RangeInclusive<i32>| {
            sides
                .map(|round| rounds.length(&set(round, None), Some(shape)))
                .collect()
        };
        (
            all(1..=i32::try_from(shape.winners).unwrap()),
            all(1..=i32::try_from(shape.losers).unwrap())
                .into_iter()
                .collect::<Vec<_>>()
                .into_iter()
                .zip(1..)
                .map(|(_, j)| rounds.length(&set(-j, None), Some(shape)))
                .collect(),
        )
    }

    #[test]
    fn top_8_and_the_grand_final_in_double_elimination() {
        // 8 players: 3 winners rounds and the grand final, 4 losers rounds;
        // every set is in the top 8.
        let eight = lengths(Shape {
            winners: 4,
            losers: 4,
        });
        assert_eq!(eight, (vec![3, 3, 3, 5], vec![3, 3, 3, 3]));
        // 16 players: the top 8 starts at the winners semi-finals and the
        // third losers round, with four players left on each side.
        let sixteen = lengths(Shape {
            winners: 5,
            losers: 6,
        });
        assert_eq!(sixteen, (vec![2, 2, 3, 3, 5], vec![2, 2, 3, 3, 3, 3]));
        // 24 and 32 players play a bracket of 32.
        let thirty_two = lengths(Shape {
            winners: 6,
            losers: 8,
        });
        assert_eq!(
            thirty_two,
            (vec![2, 2, 2, 3, 3, 5], vec![2, 2, 2, 2, 3, 3, 3, 3])
        );
    }

    #[test]
    fn top_n_and_the_final_in_single_elimination() {
        // 16 players: quarter-finals hold 8.
        let sixteen = lengths(Shape {
            winners: 4,
            losers: 0,
        });
        assert_eq!(sixteen, (vec![2, 3, 3, 5], vec![]));
        // 32 players: the final is the grand final.
        let thirty_two = lengths(Shape {
            winners: 5,
            losers: 0,
        });
        assert_eq!(thirty_two, (vec![2, 2, 3, 3, 5], vec![]));
    }

    #[test]
    fn top_6_splits_two_winners_and_four_losers() {
        let shape = Shape {
            winners: 5,
            losers: 6,
        };
        assert!(shape.in_top(6, 4) && !shape.in_top(6, 3));
        assert!(shape.in_top(6, -3) && !shape.in_top(6, -2));
    }

    #[test]
    fn a_rule_wins_over_the_top_and_no_shape_means_the_default() {
        let rounds = Rounds {
            games_to_win: 2,
            rules: vec![rule(4, None, Side::Winners, 1)],
            top: Some(Top {
                top: 8,
                games_to_win: 3,
            }),
            grand_final: None,
        };
        let shape = Some(Shape {
            winners: 5,
            losers: 6,
        });
        assert_eq!(rounds.length(&set(4, None), shape), 1);
        assert_eq!(rounds.length(&set(3, None), shape), 3);
        assert_eq!(rounds.length(&set(3, None), None), 2);
    }

    #[test]
    fn the_shape_is_read_per_phase() {
        let top = Phase::named("Top 8");
        let sets = [set(3, None), set(-4, None), set(2, Some(top.clone()))];
        assert_eq!(
            Shape::of(&sets, None),
            Shape {
                winners: 3,
                losers: 4
            }
        );
        assert_eq!(
            Shape::of(&sets, Some(&top)),
            Shape {
                winners: 2,
                losers: 0
            }
        );
    }
}
