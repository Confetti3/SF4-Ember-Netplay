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
    /// How many games `set` is first to: the first rule that covers it, or
    /// `games_to_win`.
    pub fn length(&self, set: &Set) -> u8 {
        self.rules
            .iter()
            .find(|rule| rule.covers(set))
            .map_or(self.games_to_win, |rule| rule.games_to_win)
    }

    /// Whether any rule is for one phase, so the phase of each set matters.
    pub fn names_phases(&self) -> bool {
        self.rules.iter().any(|rule| rule.phase.is_some())
    }

    pub(crate) fn check(&self) -> Result<(), String> {
        let lengths_valid = std::iter::once(self.games_to_win)
            .chain(self.rules.iter().map(|rule| rule.games_to_win))
            .all(games_to_win_valid);
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
        };
        let lengths: Vec<u8> = [1, 2, 3, 4, 5, 9, -1, -2, -3, -7]
            .into_iter()
            .map(|round| rounds.length(&set(round, None)))
            .collect();
        assert_eq!(lengths, [2, 2, 3, 3, 4, 4, 1, 1, 3, 3]);
        // Round 0 is not a losers round.
        assert_eq!(rounds.length(&set(0, None)), 2);
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
        };
        let pools = phase("Pools", Some("1300001"));
        let top = phase("Top 8", Some("1300002"));
        let length = |round, phase: Option<&Phase>| rounds.length(&set(round, phase.cloned()));
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
        };
        assert_eq!(by_id.length(&set(1, Some(top.clone()))), 3);
        assert_eq!(by_id.length(&set(1, Some(pools))), 2);
        // A phase known only by its ID is named by it.
        let nameless = Phase::new(None, Some("1300002".into())).unwrap();
        assert_eq!(by_id.length(&set(1, Some(nameless))), 3);
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
        };
        assert!(with(vec![rule(1, None, Side::Any, 10)]).check().is_ok());
        assert!(with(vec![rule(0, None, Side::Any, 3)]).check().is_err());
        assert!(with(vec![rule(4, Some(3), Side::Any, 3)]).check().is_err());
        assert!(with(vec![rule(1, None, Side::Any, 11)]).check().is_err());
        assert!(
            Rounds {
                games_to_win: 0,
                rules: vec![]
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
                        rules: vec![rule(1, None, Side::Any, 3)]
                    },
                    &read
                )
                .is_empty()
        );
        // Sets with no phase at all are said too.
        let said = PhaseWatch::default().read(&rounds, &[set(1, None)]);
        assert!(said[0].contains("phases seen: none;"), "{said:?}");
    }
}
