//! The points boards of embernetplay.link (docs/design/SITE_PLAYER_CARDS.md,
//! "Points boards"): the listed players by their account's points (BP), and
//! for each fighter by that fighter's points (FP). A board is a count of
//! games played put in order, never a ranking of skill: the class board is
//! that (`ranking::board`).
//!
//! Who is on them is who the class board may list (`ranking::listed`): an
//! account that takes part in ranking and sent a name, whether or not its
//! class has settled. A `Snapshot` reads all 45 boards at once, so a page of
//! any of them and a player's place on each come from the same moment.
use std::collections::HashMap;

use ember_protocol::levels::FIGHTERS;
use rusqlite::Transaction;
use serde::Serialize;

use super::{
    levels,
    ranking::{self, Listed},
};
use crate::error::Result;

/// One of the boards.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Board {
    /// The account's points (BP).
    Account,
    /// One fighter's points (FP), 0 to `FIGHTERS` - 1.
    Fighter(u8),
}

impl Board {
    /// How many boards there are.
    pub const COUNT: usize = 1 + FIGHTERS as usize;

    /// The board of `fighter`, or the account's without one. None for a
    /// fighter outside the roster.
    pub fn of(fighter: Option<u8>) -> Option<Self> {
        match fighter {
            None => Some(Self::Account),
            Some(fighter) => (fighter < FIGHTERS).then_some(Self::Fighter(fighter)),
        }
    }

    /// Every board: the account's, then the fighters' in order.
    fn all() -> impl Iterator<Item = Self> {
        std::iter::once(Self::Account).chain((0..FIGHTERS).map(Self::Fighter))
    }

    /// The board's number, 0 to `COUNT` - 1, in the order of `all`.
    pub fn slot(self) -> usize {
        match self {
            Self::Account => 0,
            Self::Fighter(fighter) => 1 + usize::from(fighter),
        }
    }
}

/// One line of a board.
pub struct Line {
    /// 1 for the most points. Players with the same points share a place.
    pub place: u32,
    /// Which of the snapshot's players (`Snapshot::player`).
    player: usize,
    pub points: u64,
    pub tier: u8,
}

/// A player's place on every board they are on, as the website is answered
/// it: each fighter as `[fighter, place]`.
#[derive(Clone, Default, PartialEq, Eq, Debug, Serialize)]
pub struct Places {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub account: Option<u32>,
    /// Ascending by fighter.
    pub fighters: Vec<(u8, u32)>,
}

/// Every board, read at once.
pub struct Snapshot {
    read_at: u64,
    players: Vec<Listed>,
    /// By `Board::slot`: each board's lines, the most points first, then by
    /// name.
    boards: Vec<Vec<Line>>,
    /// By account, for everyone on a board.
    places: HashMap<String, Places>,
}

impl Snapshot {
    /// The boards as they stand at `now`. `classes_shown` is the bridge's
    /// `rooms.skill_badges`: while it is off no player has a class.
    pub fn read(
        tx: &Transaction<'_>,
        developers: &[String],
        classes_shown: bool,
        now: u64,
    ) -> Result<Self> {
        let mut players = ranking::listed(tx, developers, now)?;
        if !classes_shown {
            players.iter_mut().for_each(|player| player.class = None);
        }
        let mut boards: Vec<Vec<Line>> = (0..Board::COUNT).map(|_| Vec::new()).collect();
        {
            let listed: HashMap<&str, usize> = players
                .iter()
                .enumerate()
                .map(|(index, player)| (player.account.as_str(), index))
                .collect();
            for counted in levels::all_points(tx)? {
                let on = (
                    listed.get(counted.account.as_str()),
                    Board::of(counted.fighter),
                );
                if let (Some(player), Some(board)) = on {
                    boards[board.slot()].push(Line {
                        place: 0,
                        player: *player,
                        points: counted.points,
                        tier: counted.tier,
                    });
                }
            }
        }
        let mut places: HashMap<String, Places> = HashMap::new();
        for board in Board::all() {
            let lines = &mut boards[board.slot()];
            lines.sort_by(|one, other| {
                let (one_player, other_player) = (&players[one.player], &players[other.player]);
                other
                    .points
                    .cmp(&one.points)
                    .then_with(|| one_player.listing.name.cmp(&other_player.listing.name))
                    .then_with(|| one_player.account.cmp(&other_player.account))
            });
            // One more than the players with more points, so equal points
            // share a place.
            let mut place = 0;
            let mut above = None;
            for (index, line) in lines.iter_mut().enumerate() {
                if above != Some(line.points) {
                    place = u32::try_from(index + 1).unwrap_or(u32::MAX);
                    above = Some(line.points);
                }
                line.place = place;
                let of = places
                    .entry(players[line.player].account.clone())
                    .or_default();
                // The fighters' boards come in order, so each player's
                // fighters come out ascending.
                match board {
                    Board::Account => of.account = Some(place),
                    Board::Fighter(fighter) => of.fighters.push((fighter, place)),
                }
            }
        }
        Ok(Self {
            read_at: now,
            players,
            boards,
            places,
        })
    }

    /// When the boards were read, unix seconds.
    pub fn read_at(&self) -> u64 {
        self.read_at
    }

    /// Every line of `board`, in order.
    pub fn lines(&self, board: Board) -> &[Line] {
        &self.boards[board.slot()]
    }

    /// Whose line `line`, one of this snapshot's, is.
    pub fn player(&self, line: &Line) -> &Listed {
        &self.players[line.player]
    }

    /// `account`'s place on every board it is on.
    pub fn places(&self, account: &str) -> Places {
        self.places.get(account).cloned().unwrap_or_default()
    }
}
