//! The bracket layout: every node of a bracket, from the format and the
//! number of entrants alone.
use crate::{Error, Result};

use super::{Format, Side};

/// Where a node's slot gets its player.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Source {
    /// A bye: nobody comes from here.
    Empty,
    Seed(usize),
    Winner(usize),
    Loser(usize),
}

impl Source {
    pub(super) fn fed_by(self) -> Option<usize> {
        match self {
            Self::Winner(node) | Self::Loser(node) => Some(node),
            _ => None,
        }
    }
}

/// One set in the bracket. Sources always name earlier nodes, so walking the
/// plan in order settles every slot whose source is decided.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Node {
    pub side: Side,
    pub round: u32,
    pub position: u32,
    pub label: String,
    pub sources: [Source; 2],
    /// The loser leaves the tournament (single elimination, the losers
    /// bracket and the grand final). A winners-bracket loser drops instead.
    pub eliminates: bool,
    /// How far a player who leaves here got: later stages rank higher.
    pub stage: u32,
    /// Played at the finals length.
    pub finals: bool,
    /// The grand final reset, played only when the losers-bracket player
    /// (slot 1) wins the named grand final.
    pub reset_of: Option<usize>,
}

/// The standard seed order for a bracket of `size` (a power of two), so the
/// top seeds meet as late as possible: 1, 8, 4, 5, 2, 7, 3, 6 for eight.
pub(super) fn seed_order(size: usize) -> Vec<usize> {
    let mut order = vec![1];
    while order.len() < size {
        let next = order.len() * 2;
        order = order
            .iter()
            .flat_map(|&seed| [seed, next + 1 - seed])
            .collect();
    }
    order
}

fn elimination_label(round: u32, rounds: u32) -> String {
    match rounds - round {
        0 => "Final".into(),
        1 => "Semifinals".into(),
        2 => "Quarterfinals".into(),
        _ => format!("Round {round}"),
    }
}

fn side_label(side: &str, round: u32, rounds: u32) -> String {
    match rounds - round {
        0 => format!("{side} final"),
        1 => format!("{side} semifinals"),
        _ => format!("{side} round {round}"),
    }
}

/// Every node of a bracket for `entrants` players.
pub fn plan(format: Format, entrants: usize, grand_final_reset: bool) -> Result<Vec<Node>> {
    if entrants < 2 || entrants > format.max_entrants() {
        return Err(Error::InvalidField("entrants"));
    }
    if format == Format::RoundRobin {
        return Ok(round_robin(entrants));
    }
    let size = entrants.next_power_of_two();
    let rounds = size.trailing_zeros();
    let double = format == Format::DoubleElimination;
    let losers_rounds = if double { 2 * (rounds - 1) } else { 0 };
    let mut nodes = Vec::new();
    // winners[r][i] is the node index of winners round r + 1, position i.
    let mut winners: Vec<Vec<usize>> = Vec::new();
    let order = seed_order(size);
    for round in 1..=rounds {
        let count = size >> round;
        let mut row = Vec::with_capacity(count);
        for position in 0..count {
            let sources = if round == 1 {
                [order[2 * position], order[2 * position + 1]].map(|seed| {
                    if seed <= entrants {
                        Source::Seed(seed - 1)
                    } else {
                        Source::Empty
                    }
                })
            } else {
                let previous = &winners[round as usize - 2];
                [
                    Source::Winner(previous[2 * position]),
                    Source::Winner(previous[2 * position + 1]),
                ]
            };
            let (label, stage) = if double {
                // A winners-bracket loser drops into losers round 1 (from
                // round 1) or 2(r - 1) (from round r); the winners final's
                // loser goes to the grand final when there is no losers bracket.
                let drop = if round == 1 || rounds == 1 {
                    1
                } else {
                    2 * (round - 1)
                };
                (side_label("Winners", round, rounds), drop)
            } else {
                (elimination_label(round, rounds), round)
            };
            row.push(nodes.len());
            nodes.push(Node {
                side: Side::Winners,
                round,
                position: position as u32,
                label,
                sources,
                eliminates: !double,
                stage,
                finals: !double && round == rounds,
                reset_of: None,
            });
        }
        winners.push(row);
    }
    if !double {
        return Ok(nodes);
    }
    let mut previous: Vec<usize> = Vec::new();
    for round in 1..=losers_rounds {
        let mut row = Vec::new();
        if round == 1 {
            let first = &winners[0];
            for position in 0..first.len() / 2 {
                row.push([
                    Source::Loser(first[2 * position]),
                    Source::Loser(first[2 * position + 1]),
                ]);
            }
        } else if round % 2 == 0 {
            // Players dropping from winners round j + 1 meet the survivors of
            // losers round 2j - 1, in an order that keeps them away from the
            // player who just beat them: reversed on odd j, halves swapped on even j.
            let j = (round / 2) as usize;
            let dropping = &winners[j];
            let count = previous.len();
            for (position, &survivor) in previous.iter().enumerate() {
                let from = if j % 2 == 1 {
                    count - 1 - position
                } else {
                    (position + count / 2) % count
                };
                row.push([Source::Winner(survivor), Source::Loser(dropping[from])]);
            }
        } else {
            for position in 0..previous.len() / 2 {
                row.push([
                    Source::Winner(previous[2 * position]),
                    Source::Winner(previous[2 * position + 1]),
                ]);
            }
        }
        previous = row
            .into_iter()
            .enumerate()
            .map(|(position, sources)| {
                nodes.push(Node {
                    side: Side::Losers,
                    round,
                    position: position as u32,
                    label: side_label("Losers", round, losers_rounds),
                    sources,
                    eliminates: true,
                    stage: round,
                    finals: false,
                    reset_of: None,
                });
                nodes.len() - 1
            })
            .collect();
    }
    let winners_final = *winners
        .last()
        .and_then(|row| row.first())
        .ok_or(Error::InvalidField("entrants"))?;
    let losers_side = previous
        .first()
        .map_or(Source::Loser(winners_final), |&node| Source::Winner(node));
    let stage = losers_rounds + 1;
    let grand_final = nodes.len();
    nodes.push(Node {
        side: Side::GrandFinal,
        round: 1,
        position: 0,
        label: "Grand final".into(),
        sources: [Source::Winner(winners_final), losers_side],
        eliminates: true,
        stage,
        finals: true,
        reset_of: None,
    });
    if grand_final_reset {
        nodes.push(Node {
            side: Side::GrandFinal,
            round: 2,
            position: 0,
            label: "Grand final reset".into(),
            sources: [Source::Loser(grand_final), Source::Winner(grand_final)],
            eliminates: true,
            stage,
            finals: true,
            reset_of: Some(grand_final),
        });
    }
    Ok(nodes)
}

/// The circle method: every pair once, each round as even as the count allows.
fn round_robin(entrants: usize) -> Vec<Node> {
    let mut ids: Vec<Option<usize>> = (0..entrants).map(Some).collect();
    if ids.len() % 2 == 1 {
        ids.push(None);
    }
    let count = ids.len();
    let mut nodes = Vec::new();
    for round in 1..count as u32 {
        let mut position = 0;
        for index in 0..count / 2 {
            if let (Some(a), Some(b)) = (ids[index], ids[count - 1 - index]) {
                let (a, b) = (a.min(b), a.max(b));
                nodes.push(Node {
                    side: Side::RoundRobin,
                    round,
                    position,
                    label: format!("Round {round}"),
                    sources: [Source::Seed(a), Source::Seed(b)],
                    eliminates: false,
                    stage: 0,
                    finals: false,
                    reset_of: None,
                });
                position += 1;
            }
        }
        let last = ids.pop().flatten();
        ids.insert(1, last);
    }
    nodes
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tournament::MAX_ROUND_ROBIN;

    #[test]
    fn seed_order_is_standard() {
        assert_eq!(seed_order(8), vec![1, 8, 4, 5, 2, 7, 3, 6]);
        assert_eq!(
            seed_order(16),
            vec![1, 16, 8, 9, 4, 13, 5, 12, 2, 15, 7, 10, 3, 14, 6, 11]
        );
    }

    #[test]
    fn plans_have_the_expected_shape() {
        let single = plan(Format::SingleElimination, 8, true).unwrap();
        assert_eq!(single.len(), 7);
        assert_eq!(single.last().unwrap().label, "Final");
        assert_eq!(single[4].label, "Semifinals");
        // 7 winners, 2 + 2 + 1 + 1 losers, grand final and reset.
        let double = plan(Format::DoubleElimination, 8, true).unwrap();
        assert_eq!(double.len(), 15);
        assert_eq!(
            double
                .iter()
                .filter(|node| node.side == Side::Losers)
                .count(),
            6
        );
        assert_eq!(
            plan(Format::DoubleElimination, 16, false).unwrap().len(),
            30
        );
        let robin = plan(Format::RoundRobin, 5, true).unwrap();
        assert_eq!(robin.len(), 10);
        assert_eq!(robin.iter().map(|node| node.round).max(), Some(5));
        assert!(plan(Format::SingleElimination, 1, true).is_err());
        assert!(plan(Format::RoundRobin, 33, true).is_err());
        assert!(plan(Format::DoubleElimination, 129, true).is_err());
        for format in Format::ALL {
            for entrants in 2..=33.min(format.max_entrants()) {
                for (index, node) in plan(format, entrants, true).unwrap().iter().enumerate() {
                    for source in node.sources {
                        assert!(source.fed_by().is_none_or(|from| from < index));
                    }
                }
            }
        }
    }

    #[test]
    fn round_robin_pairs_everyone_once() {
        for entrants in 2..=MAX_ROUND_ROBIN {
            let nodes = plan(Format::RoundRobin, entrants, true).unwrap();
            let mut pairs = std::collections::BTreeSet::new();
            for node in &nodes {
                let [Source::Seed(a), Source::Seed(b)] = node.sources else {
                    panic!()
                };
                assert!(a < b && pairs.insert((a, b)));
            }
            assert_eq!(pairs.len(), entrants * (entrants - 1) / 2);
            // Nobody plays twice in one round.
            for round in 1..=entrants as u32 {
                let mut seen = std::collections::BTreeSet::new();
                for node in nodes.iter().filter(|node| node.round == round) {
                    for source in node.sources {
                        let Source::Seed(entrant) = source else {
                            panic!()
                        };
                        assert!(seen.insert(entrant));
                    }
                }
            }
        }
    }
}
