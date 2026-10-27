//! Tournaments: brackets the bridge runs on a provider connection (an
//! extension beyond EMBER-TB-001, which leaves multi-match scheduling to the
//! provider).
//!
//! This module is pure. The wire commands and enums are here; `layout` and
//! `engine` hold the bracket the bridge runs, and every item stays reachable
//! at `tournament::`. [`plan`] lays out every node of a bracket from the
//! format and the number of entrants, deterministically, so the bridge stores
//! only each node's state. [`settle`], [`complete`] and [`reopen`] move those
//! states forward; the bridge creates a match for each node that becomes
//! ready and reports its result back. Entrants are numbered by seed, 0 first.
use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use crate::{
    EmberId, Error, Result,
    encoding::is_prefixed_id,
    matches::{check_provider_head, check_provider_tail, games_to_win_valid},
    wire_enum::wire_enum,
};

mod engine;
mod layout;

pub use engine::{
    Blocked, NodeState, Slot, complete, eliminated, finished, placements, reopen, settle,
};
pub use layout::{Node, Source, plan};

/// Entrants in an elimination bracket at most.
pub const MAX_ENTRANTS: usize = 128;
/// Entrants in a round robin at most (496 matches).
pub const MAX_ROUND_ROBIN: usize = 32;

wire_enum! {
    serde
    #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
    pub enum Format {
        SingleElimination = "single_elimination",
        /// A winners and a losers bracket, then a grand final.
        DoubleElimination = "double_elimination",
        /// Everyone plays everyone once.
        RoundRobin = "round_robin",
    }
}

impl Format {
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
        check_provider_head(
            &self.external_tournament_id,
            "external_tournament_id",
            &self.game,
        )?;
        if !games_to_win_valid(self.games_to_win) {
            return Err(Error::InvalidField("games_to_win"));
        }
        if self
            .finals_games_to_win
            .is_some_and(|games| !games_to_win_valid(games))
        {
            return Err(Error::InvalidField("finals_games_to_win"));
        }
        check_provider_tail(&self.required_build_id, &self.metadata)
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

wire_enum! {
    serde
    #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
    pub enum Side {
        Winners = "winners",
        Losers = "losers",
        GrandFinal = "grand_final",
        RoundRobin = "round_robin",
    }
}

wire_enum! {
    serde
    #[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
    pub enum Status {
        /// A slot is still open.
        #[default]
        Pending = "pending",
        /// Both players are known and the set can be played.
        Ready = "ready",
        /// Its match exists.
        Playing = "playing",
        Played = "played",
        /// One player and an empty slot.
        Bye = "bye",
        /// One player withdrew.
        Walkover = "walkover",
        /// Nobody plays it: both slots empty, or a reset that is not needed.
        Skipped = "skipped",
    }
}

impl Status {
    pub fn decided(self) -> bool {
        matches!(
            self,
            Self::Played | Self::Bye | Self::Walkover | Self::Skipped
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
