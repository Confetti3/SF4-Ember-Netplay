//! The bracket engine: the states of a plan's nodes and the rules that move
//! them forward.
use crate::{Error, Result};

use super::{
    Side, Status,
    layout::{Node, Source},
};

/// A slot's player, once its source is decided.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Slot {
    #[default]
    Open,
    /// Nobody comes: a bye, or a withdrawn player's empty place.
    Empty,
    Entrant(usize),
}

impl Slot {
    pub fn entrant(self) -> Option<usize> {
        match self {
            Self::Entrant(entrant) => Some(entrant),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NodeState {
    pub slots: [Slot; 2],
    pub status: Status,
    /// The winning slot, once decided.
    pub winner: Option<u8>,
}

impl NodeState {
    pub fn winner(&self) -> Option<usize> {
        self.winner
            .and_then(|slot| self.slots[usize::from(slot)].entrant())
    }

    pub fn loser(&self) -> Option<usize> {
        self.winner
            .and_then(|slot| self.slots[usize::from(1 - slot)].entrant())
    }
}

fn resolve(states: &[NodeState], source: Source) -> Option<Slot> {
    let decided = |node: usize| states[node].status.decided();
    match source {
        Source::Empty => Some(Slot::Empty),
        Source::Seed(entrant) => Some(Slot::Entrant(entrant)),
        Source::Winner(node) if decided(node) => {
            Some(states[node].winner().map_or(Slot::Empty, Slot::Entrant))
        }
        Source::Loser(node) if decided(node) => {
            Some(states[node].loser().map_or(Slot::Empty, Slot::Entrant))
        }
        _ => None,
    }
}

/// Fills every slot whose source is decided and decides what needs no match:
/// byes, walkovers against withdrawn players, empty sets and an unneeded
/// reset. A playing node with a withdrawn player becomes a walkover; the
/// bridge cancels its match. Returns the nodes that changed.
pub fn settle(plan: &[Node], states: &mut [NodeState], withdrawn: &[bool]) -> Vec<usize> {
    let mut changed = Vec::new();
    let live = |slot: Slot| slot.entrant().is_some_and(|entrant| !withdrawn[entrant]);
    for index in 0..plan.len() {
        let before = states[index];
        let node = &plan[index];
        if states[index].status == Status::Pending {
            for slot in 0..2 {
                if states[index].slots[slot] == Slot::Open
                    && let Some(value) = resolve(states, node.sources[slot])
                {
                    states[index].slots[slot] = value;
                }
            }
        }
        // The reset is not played when the winners-bracket player (slot 0)
        // won the grand final, by result or by walkover.
        let unneeded = node.reset_of.is_some_and(|grand_final| {
            states[grand_final].status.decided() && states[grand_final].winner != Some(1)
        });
        let state = &mut states[index];
        let open = state.slots.contains(&Slot::Open);
        if state.status == Status::Pending && !open && unneeded {
            state.status = Status::Skipped;
            state.winner = None;
        } else if matches!(
            state.status,
            Status::Pending | Status::Ready | Status::Playing
        ) && !open
        {
            match (live(state.slots[0]), live(state.slots[1])) {
                (true, true) => {
                    if state.status == Status::Pending {
                        state.status = Status::Ready;
                    }
                }
                (true, false) | (false, true) => {
                    let winner = if live(state.slots[0]) { 0 } else { 1 };
                    let other = state.slots[1 - winner];
                    state.status = if other.entrant().is_some() {
                        Status::Walkover
                    } else {
                        Status::Bye
                    };
                    state.winner = Some(winner as u8);
                }
                (false, false) => {
                    state.status = Status::Skipped;
                    state.winner = None;
                }
            }
        }
        if states[index] != before {
            changed.push(index);
        }
    }
    changed
}

/// Records the result of a playing node.
pub fn complete(states: &mut [NodeState], node: usize, winner_slot: u8) -> Result<()> {
    let state = states.get_mut(node).ok_or(Error::InvalidField("node"))?;
    if state.status != Status::Playing || winner_slot > 1 {
        return Err(Error::InvalidField("node"));
    }
    state.status = Status::Played;
    state.winner = Some(winner_slot);
    Ok(())
}

/// Why a result cannot be reopened.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Blocked {
    /// The node is not a played set.
    NotPlayed,
    /// A later set fed by this one has been played, or has games recorded.
    Downstream(usize),
}

/// Takes back a played node's result so its set can be corrected. Every later
/// node it fed is cleared again, recursing through byes and walkovers; a later
/// set that is playing but has no games is returned so the bridge cancels its
/// match. Nothing changes when a later set has a game recorded or is played.
pub fn reopen(
    plan: &[Node],
    states: &mut [NodeState],
    node: usize,
    has_games: impl Fn(usize) -> bool,
) -> std::result::Result<Vec<usize>, Blocked> {
    if states.get(node).map(|state| state.status) != Some(Status::Played) {
        return Err(Blocked::NotPlayed);
    }
    let mut work = states.to_vec();
    let mut cancelled = Vec::new();
    unwind(plan, &mut work, node, &has_games, &mut cancelled)?;
    work[node].status = Status::Playing;
    work[node].winner = None;
    states.copy_from_slice(&work);
    Ok(cancelled)
}

fn unwind(
    plan: &[Node],
    states: &mut [NodeState],
    node: usize,
    has_games: &impl Fn(usize) -> bool,
    cancelled: &mut Vec<usize>,
) -> std::result::Result<(), Blocked> {
    for later in node + 1..plan.len() {
        let fed: Vec<usize> = (0..2)
            .filter(|&slot| plan[later].sources[slot].fed_by() == Some(node))
            .collect();
        if fed.is_empty() && plan[later].reset_of != Some(node) {
            continue;
        }
        match states[later].status {
            Status::Pending | Status::Ready => {}
            Status::Playing => {
                if has_games(later) {
                    return Err(Blocked::Downstream(later));
                }
                cancelled.push(later);
            }
            Status::Played => return Err(Blocked::Downstream(later)),
            Status::Bye | Status::Walkover | Status::Skipped => {
                // A walkover can follow a match that had games before the
                // withdrawal cancelled it; those games still count.
                if has_games(later) {
                    return Err(Blocked::Downstream(later));
                }
                unwind(plan, states, later, has_games, cancelled)?;
            }
        }
        for slot in fed {
            states[later].slots[slot] = Slot::Open;
        }
        states[later].status = Status::Pending;
        states[later].winner = None;
    }
    Ok(())
}

/// The player a decided node sends out of the tournament, if any. The
/// winners-bracket player who loses a grand final that has a reset plays the
/// reset instead of leaving.
pub fn eliminated(plan: &[Node], states: &[NodeState], node: usize) -> Option<usize> {
    let state = states.get(node)?;
    if !matches!(state.status, Status::Played | Status::Walkover) || !plan[node].eliminates {
        return None;
    }
    let resettable = plan.iter().any(|later| later.reset_of == Some(node));
    if resettable && state.winner == Some(1) {
        return None;
    }
    state.loser()
}

/// Every node is decided.
pub fn finished(states: &[NodeState]) -> bool {
    states.iter().all(|state| state.status.decided())
}

/// Places in a finished elimination bracket, by entrant: one more than the
/// number of players who got strictly further, so players who went out at
/// the same stage share a place. `None` while the bracket is running.
pub fn placements(plan: &[Node], states: &[NodeState], entrants: usize) -> Option<Vec<u32>> {
    if !finished(states)
        || plan
            .first()
            .is_some_and(|node| node.side == Side::RoundRobin)
    {
        return None;
    }
    let champion = (0..plan.len())
        .rev()
        .find(|&node| states[node].status != Status::Skipped)
        .and_then(|node| states[node].winner());
    let mut stage = vec![0u32; entrants];
    for (index, (node, state)) in plan.iter().zip(states).enumerate() {
        if let Some(loser) = eliminated(plan, states, index) {
            stage[loser] = stage[loser].max(node.stage);
        }
        if state.status == Status::Skipped {
            // Withdrawn players with nobody to forfeit to.
            for entrant in state.slots.iter().filter_map(|slot| slot.entrant()) {
                stage[entrant] = stage[entrant].max(node.stage);
            }
        }
    }
    if let Some(champion) = champion {
        stage[champion] = u32::MAX;
    }
    Some(
        (0..entrants)
            .map(|entrant| {
                1 + stage
                    .iter()
                    .filter(|&&other| other > stage[entrant])
                    .count() as u32
            })
            .collect(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tournament::{Format, layout::plan};

    fn start(plan: &[Node], entrants: usize) -> (Vec<NodeState>, Vec<bool>) {
        let mut states = vec![NodeState::default(); plan.len()];
        let withdrawn = vec![false; entrants];
        settle(plan, &mut states, &withdrawn);
        (states, withdrawn)
    }

    /// Plays every ready set with `pick` choosing the winning slot, until done.
    fn run(
        plan: &[Node],
        states: &mut [NodeState],
        withdrawn: &[bool],
        pick: impl Fn(usize, [usize; 2]) -> u8,
    ) {
        for _ in 0..plan.len() * 2 {
            let ready: Vec<usize> = (0..plan.len())
                .filter(|&node| states[node].status == Status::Ready)
                .collect();
            if ready.is_empty() {
                break;
            }
            for node in ready {
                states[node].status = Status::Playing;
                let players = states[node].slots.map(|slot| slot.entrant().unwrap());
                complete(states, node, pick(node, players)).unwrap();
            }
            settle(plan, states, withdrawn);
        }
    }

    /// The better seed (lower number) always wins.
    fn seeds_win(_: usize, players: [usize; 2]) -> u8 {
        u8::from(players[1] < players[0])
    }

    #[test]
    fn byes_go_to_the_top_seeds() {
        for entrants in [3, 5, 6, 7, 9, 17] {
            for format in [Format::SingleElimination, Format::DoubleElimination] {
                let nodes = plan(format, entrants, true).unwrap();
                let (states, _) = start(&nodes, entrants);
                let byes: Vec<usize> = (0..nodes.len())
                    .filter(|&node| {
                        states[node].status == Status::Bye && nodes[node].side == Side::Winners
                    })
                    .filter_map(|node| states[node].winner())
                    .collect();
                let expected = entrants.next_power_of_two() - entrants;
                assert_eq!(byes.len(), expected, "{format:?} {entrants}");
                assert!(
                    byes.iter().all(|&seed| seed < expected),
                    "{format:?} {entrants}: {byes:?}"
                );
            }
        }
    }

    #[test]
    fn single_elimination_places_by_round() {
        let nodes = plan(Format::SingleElimination, 8, true).unwrap();
        let (mut states, withdrawn) = start(&nodes, 8);
        run(&nodes, &mut states, &withdrawn, seeds_win);
        let places = placements(&nodes, &states, 8).unwrap();
        assert_eq!(places, vec![1, 2, 3, 3, 5, 5, 5, 5]);
    }

    #[test]
    fn double_elimination_plays_every_entrant_to_two_losses() {
        for entrants in [2, 3, 4, 5, 6, 7, 8, 9, 12, 16, 17, 32] {
            for pick in [
                seeds_win as fn(usize, [usize; 2]) -> u8,
                |node, _| (node % 2) as u8,
                |_, players| u8::from(players[1] > players[0]),
            ] {
                let nodes = plan(Format::DoubleElimination, entrants, true).unwrap();
                let (mut states, withdrawn) = start(&nodes, entrants);
                run(&nodes, &mut states, &withdrawn, pick);
                assert!(finished(&states), "{entrants}");
                let mut losses = vec![0; entrants];
                for state in &states {
                    if state.status == Status::Played {
                        losses[state.loser().unwrap()] += 1;
                    }
                }
                let places = placements(&nodes, &states, entrants).unwrap();
                let champion = places.iter().position(|&place| place == 1).unwrap();
                assert_eq!(places.iter().filter(|&&place| place == 1).count(), 1);
                for (entrant, &lost) in losses.iter().enumerate() {
                    if entrant == champion {
                        assert!(lost <= 1, "{entrants}: champion lost {}", lost);
                    } else {
                        assert_eq!(lost, 2, "{entrants}: entrant {entrant}");
                    }
                }
                assert_eq!(places.iter().filter(|&&place| place == 2).count(), 1);
            }
        }
    }

    #[test]
    fn eight_entrant_double_elimination_places() {
        let nodes = plan(Format::DoubleElimination, 8, true).unwrap();
        let (mut states, withdrawn) = start(&nodes, 8);
        run(&nodes, &mut states, &withdrawn, seeds_win);
        // The grand final reset is skipped when the winners side wins.
        assert_eq!(states.last().unwrap().status, Status::Skipped);
        assert_eq!(
            placements(&nodes, &states, 8).unwrap(),
            vec![1, 2, 3, 4, 5, 5, 7, 7]
        );
    }

    #[test]
    fn the_first_drop_avoids_a_rematch() {
        // With the better seed winning, nobody who drops from winners round 2
        // meets in losers round 2 the player they beat in winners round 1.
        for entrants in [8, 16, 32] {
            let nodes = plan(Format::DoubleElimination, entrants, true).unwrap();
            let (mut states, withdrawn) = start(&nodes, entrants);
            run(&nodes, &mut states, &withdrawn, seeds_win);
            let mut met = std::collections::BTreeSet::new();
            for (node, state) in nodes.iter().zip(&states) {
                if node.side == Side::Winners && node.round == 1 {
                    let [a, b] = state.slots.map(|slot| slot.entrant().unwrap());
                    met.insert((a.min(b), a.max(b)));
                }
            }
            for (node, state) in nodes.iter().zip(&states) {
                if node.side == Side::Losers && node.round == 2 {
                    let [a, b] = state.slots.map(|slot| slot.entrant().unwrap());
                    assert!(
                        !met.contains(&(a.min(b), a.max(b))),
                        "{entrants}: {a} and {b}"
                    );
                }
            }
        }
    }

    #[test]
    fn the_losers_side_forces_a_reset() {
        let nodes = plan(Format::DoubleElimination, 4, true).unwrap();
        let (mut states, withdrawn) = start(&nodes, 4);
        let grand_final = nodes
            .iter()
            .position(|node| node.label == "Grand final")
            .unwrap();
        // Seeds win everywhere except the grand final, which slot 1 takes.
        run(&nodes, &mut states, &withdrawn, |node, players| {
            if node == grand_final {
                1
            } else {
                seeds_win(node, players)
            }
        });
        let reset = states.last().unwrap();
        assert_eq!(reset.status, Status::Played);
        let places = placements(&nodes, &states, 4).unwrap();
        assert_eq!(places.iter().filter(|&&place| place <= 2).count(), 2);
        // Without a reset the grand final decides it.
        let nodes = plan(Format::DoubleElimination, 4, false).unwrap();
        let (mut states, withdrawn) = start(&nodes, 4);
        let grand_final = nodes.len() - 1;
        run(&nodes, &mut states, &withdrawn, |node, players| {
            if node == grand_final {
                1
            } else {
                seeds_win(node, players)
            }
        });
        let places = placements(&nodes, &states, 4).unwrap();
        assert_eq!(places[states[grand_final].winner().unwrap()], 1);
    }

    #[test]
    fn a_withdrawal_hands_over_every_later_set() {
        let nodes = plan(Format::DoubleElimination, 8, true).unwrap();
        let (mut states, mut withdrawn) = start(&nodes, 8);
        // Seed 0 withdraws before playing: their first set becomes a walkover,
        // and the place they would take in the losers bracket is a walkover too.
        withdrawn[0] = true;
        let changed = settle(&nodes, &mut states, &withdrawn);
        assert!(changed.contains(&0));
        assert_eq!(states[0].status, Status::Walkover);
        assert_eq!(states[0].winner(), Some(7));
        run(&nodes, &mut states, &withdrawn, seeds_win);
        let places = placements(&nodes, &states, 8).unwrap();
        assert!(places[0] >= 7);
        assert_eq!(places.iter().filter(|&&place| place == 1).count(), 1);
    }

    #[test]
    fn reopening_clears_what_it_fed() {
        let nodes = plan(Format::SingleElimination, 4, true).unwrap();
        let (mut states, withdrawn) = start(&nodes, 4);
        // Semifinal 0 is played; the final waits for semifinal 1.
        states[0].status = Status::Playing;
        complete(&mut states, 0, 0).unwrap();
        settle(&nodes, &mut states, &withdrawn);
        assert_eq!(states[2].slots[0], Slot::Entrant(0));
        assert_eq!(reopen(&nodes, &mut states, 0, |_| false), Ok(vec![]));
        assert_eq!(states[0].status, Status::Playing);
        assert_eq!(states[2].slots[0], Slot::Open);
        // Both semifinals played and the final started: reopening one cancels
        // the final's match while it has no games, and is refused once it does.
        complete(&mut states, 0, 1).unwrap();
        states[1].status = Status::Playing;
        complete(&mut states, 1, 0).unwrap();
        settle(&nodes, &mut states, &withdrawn);
        assert_eq!(states[2].status, Status::Ready);
        states[2].status = Status::Playing;
        assert_eq!(
            reopen(&nodes, &mut states, 1, |_| true),
            Err(Blocked::Downstream(2))
        );
        assert_eq!(states[1].status, Status::Played);
        assert_eq!(reopen(&nodes, &mut states, 1, |_| false), Ok(vec![2]));
        assert_eq!(states[2].status, Status::Pending);
        assert_eq!(states[2].slots, [Slot::Entrant(3), Slot::Open]);
        assert_eq!(
            reopen(&nodes, &mut states, 2, |_| false),
            Err(Blocked::NotPlayed)
        );
    }

    #[test]
    fn a_walkover_with_games_blocks_reopening() {
        let nodes = plan(Format::SingleElimination, 4, true).unwrap();
        let (mut states, mut withdrawn) = start(&nodes, 4);
        for node in [0, 1] {
            states[node].status = Status::Playing;
            complete(&mut states, node, 0).unwrap();
        }
        settle(&nodes, &mut states, &withdrawn);
        states[2].status = Status::Playing;
        // The final's second player withdraws after a game was recorded.
        withdrawn[1] = true;
        settle(&nodes, &mut states, &withdrawn);
        assert_eq!(states[2].status, Status::Walkover);
        assert_eq!(
            reopen(&nodes, &mut states, 0, |node| node == 2),
            Err(Blocked::Downstream(2))
        );
        assert_eq!(states[0].status, Status::Played);
        assert_eq!(reopen(&nodes, &mut states, 0, |_| false), Ok(vec![]));
        assert_eq!(states[2].status, Status::Pending);
    }

    #[test]
    fn reopening_recurses_through_byes() {
        // Three entrants, double elimination: losers round 1 is a bye for
        // whoever loses winners round 1's played set, so reopening that set
        // clears the bye and the losers final slot it filled.
        let nodes = plan(Format::DoubleElimination, 3, true).unwrap();
        let (mut states, withdrawn) = start(&nodes, 3);
        let played = (0..nodes.len())
            .find(|&node| states[node].status == Status::Ready)
            .unwrap();
        states[played].status = Status::Playing;
        complete(&mut states, played, 0).unwrap();
        settle(&nodes, &mut states, &withdrawn);
        let bye = nodes
            .iter()
            .position(|node| node.side == Side::Losers && node.round == 1)
            .unwrap();
        assert_eq!(states[bye].status, Status::Bye);
        assert_eq!(reopen(&nodes, &mut states, played, |_| false), Ok(vec![]));
        assert_eq!(states[bye].status, Status::Pending);
        assert!(states.iter().all(|state| state.status != Status::Played));
    }
}
