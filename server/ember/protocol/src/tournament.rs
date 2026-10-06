//! Tournaments: brackets the bridge runs on a provider connection (an
//! extension beyond EMBER-TB-001, which leaves multi-match scheduling to the
//! provider).
//!
//! This module is pure. [`plan`] lays out every node of a bracket from the
//! format and the number of entrants, deterministically, so the bridge stores
//! only each node's state. [`settle`], [`complete`] and [`reopen`] move those
//! states forward; the bridge creates a match for each node that becomes
//! ready and reports its result back. Entrants are numbered by seed, 0 first.
use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use crate::{
    EmberId, Error, Result,
    encoding::is_prefixed_id,
    matches::{MAX_EXTERNAL_ID, check_metadata, games_to_win_valid},
};

/// Entrants in an elimination bracket at most.
pub const MAX_ENTRANTS: usize = 128;
/// Entrants in a round robin at most (496 matches).
pub const MAX_ROUND_ROBIN: usize = 32;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Format {
    SingleElimination,
    /// A winners and a losers bracket, then a grand final.
    DoubleElimination,
    /// Everyone plays everyone once.
    RoundRobin,
}

impl Format {
    pub const ALL: [Format; 3] = [
        Self::SingleElimination,
        Self::DoubleElimination,
        Self::RoundRobin,
    ];

    pub fn as_str(self) -> &'static str {
        match self {
            Self::SingleElimination => "single_elimination",
            Self::DoubleElimination => "double_elimination",
            Self::RoundRobin => "round_robin",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|format| format.as_str() == text)
    }

    pub fn max_entrants(self) -> usize {
        match self {
            Self::RoundRobin => MAX_ROUND_ROBIN,
            _ => MAX_ENTRANTS,
        }
    }
}

fn reset_default() -> bool {
    true
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateTournament {
    pub external_tournament_id: String,
    pub game: String,
    pub format: Format,
    pub games_to_win: u8,
    /// The set length of the final (single elimination) or the grand final
    /// and its reset (double elimination). Defaults to `games_to_win`.
    #[serde(default)]
    pub finals_games_to_win: Option<u8>,
    /// Double elimination: a second grand final when the losers-bracket
    /// player wins the first.
    #[serde(default = "reset_default")]
    pub grand_final_reset: bool,
    pub required_build_id: String,
    #[serde(default)]
    pub metadata: BTreeMap<String, String>,
}

impl CreateTournament {
    pub fn check(&self) -> Result<()> {
        if self.external_tournament_id.is_empty()
            || self.external_tournament_id.len() > MAX_EXTERNAL_ID
        {
            return Err(Error::InvalidField("external_tournament_id"));
        }
        if self.game != "usf4" {
            return Err(Error::InvalidField("game"));
        }
        if !games_to_win_valid(self.games_to_win) {
            return Err(Error::InvalidField("games_to_win"));
        }
        if self
            .finals_games_to_win
            .is_some_and(|games| !games_to_win_valid(games))
        {
            return Err(Error::InvalidField("finals_games_to_win"));
        }
        if self.required_build_id.is_empty() || self.required_build_id.len() > 128 {
            return Err(Error::InvalidField("required_build_id"));
        }
        check_metadata(&self.metadata)
    }
}

/// A player the provider registers, at their own request.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RegisterEntrant {
    pub participant_id: String,
    pub ember_id: EmberId,
}

impl RegisterEntrant {
    pub fn check(&self) -> Result<()> {
        if !is_prefixed_id(&self.participant_id, "epl") {
            return Err(Error::InvalidField("participant_id"));
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Side {
    Winners,
    Losers,
    GrandFinal,
    RoundRobin,
}

impl Side {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Winners => "winners",
            Self::Losers => "losers",
            Self::GrandFinal => "grand_final",
            Self::RoundRobin => "round_robin",
        }
    }
}

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
    fn fed_by(self) -> Option<usize> {
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
fn seed_order(size: usize) -> Vec<usize> {
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

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Status {
    /// A slot is still open.
    #[default]
    Pending,
    /// Both players are known and the set can be played.
    Ready,
    /// Its match exists.
    Playing,
    Played,
    /// One player and an empty slot.
    Bye,
    /// One player withdrew.
    Walkover,
    /// Nobody plays it: both slots empty, or a reset that is not needed.
    Skipped,
}

impl Status {
    pub const ALL: [Status; 7] = [
        Self::Pending,
        Self::Ready,
        Self::Playing,
        Self::Played,
        Self::Bye,
        Self::Walkover,
        Self::Skipped,
    ];

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Pending => "pending",
            Self::Ready => "ready",
            Self::Playing => "playing",
            Self::Played => "played",
            Self::Bye => "bye",
            Self::Walkover => "walkover",
            Self::Skipped => "skipped",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|status| status.as_str() == text)
    }

    pub fn decided(self) -> bool {
        matches!(
            self,
            Self::Played | Self::Bye | Self::Walkover | Self::Skipped
        )
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

    #[test]
    fn create_checks_its_fields() {
        let mut command = CreateTournament {
            external_tournament_id: "weekly-12".into(),
            game: "usf4".into(),
            format: Format::DoubleElimination,
            games_to_win: 2,
            finals_games_to_win: Some(3),
            grand_final_reset: true,
            required_build_id: "sf4e-1.1.0".into(),
            metadata: BTreeMap::new(),
        };
        assert!(command.check().is_ok());
        command.finals_games_to_win = Some(10);
        assert!(command.check().is_ok());
        command.finals_games_to_win = Some(11);
        assert!(command.check().is_err());
        command.finals_games_to_win = Some(4);
        command.games_to_win = 0;
        assert!(command.check().is_err());
        let wire: CreateTournament = serde_json::from_value(serde_json::json!({
            "external_tournament_id": "t", "game": "usf4", "format": "round_robin",
            "games_to_win": 1, "required_build_id": "b",
        }))
        .unwrap();
        assert!(wire.grand_final_reset && wire.finals_games_to_win.is_none());
    }
}
