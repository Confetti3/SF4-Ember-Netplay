//! The public points board: the JSON the bridge writes for the website.
use serde::{Deserialize, Serialize};

use crate::{
    Error, Result,
    rooms::{is_country, is_player_name},
    skill::SKILL_BANDS,
    cards::is_card_token,
    levels::{FIGHTERS, MAX_TIER},
};

/// The most lines of a board one answer lists.
pub const MAX_POINTS_BOARD: usize = 500;

/// One board as the website lists it.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct PointsBoard {
    pub listed_at: u64,
    /// The fighter on the board, absent for the account board.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fighter: Option<u8>,
    /// The players on the board, which can be more than `players` lists.
    pub total: usize,
    pub players: Vec<PointsLine>,
}

/// The lines a board lists.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct PointsLine {
    pub place: u32,
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub band: Option<u8>,
    pub points: u64,
    pub tier: u8,
    /// A card token: what opens this player's card for a signed-in browser.
    /// Sealed when the answer is written, so it opens for a week less the
    /// minute an answer is kept.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub card: Option<String>,
}

impl PointsBoard {
    pub fn check(&self) -> Result<()> {
        if self.players.len() > MAX_POINTS_BOARD || self.players.len() > self.total {
            return Err(Error::InvalidField("players"));
        }
        if self.fighter.is_some_and(|fighter| fighter >= FIGHTERS) {
            return Err(Error::InvalidField("fighter"));
        }
        let mut last = 0;
        for line in &self.players {
            if !is_player_name(&line.name) {
                return Err(Error::InvalidField("name"));
            }
            if line.country.as_deref().is_some_and(|country| !is_country(country)) {
                return Err(Error::InvalidField("country"));
            }
            if line.band.is_some_and(|band| !(1..=SKILL_BANDS).contains(&band)) {
                return Err(Error::InvalidField("band"));
            }
            if line.tier > MAX_TIER {
                return Err(Error::InvalidField("tier"));
            }
            if line.card.as_deref().is_some_and(|card| !is_card_token(card)) {
                return Err(Error::InvalidField("card"));
            }
            if line.place < 1 || line.place < last {
                return Err(Error::InvalidField("place"));
            }
            last = line.place;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid_board() -> PointsBoard {
        PointsBoard {
            listed_at: 1_700_000_000,
            fighter: Some(2),
            total: 2,
            players: vec![
                PointsLine {
                    place: 1,
                    name: "player1".to_owned(),
                    country: Some("US".to_owned()),
                    band: Some(3),
                    points: 10,
                    tier: 1,
                    card: None,
                },
                PointsLine {
                    place: 2,
                    name: "player2".to_owned(),
                    country: None,
                    band: None,
                    points: 8,
                    tier: 0,
                    card: None,
                },
            ],
        }
    }

    #[test]
    fn round_trips() {
        let board = valid_board();
        let json = serde_json::to_string(&board).expect("a board is written as JSON");
        let back: PointsBoard = serde_json::from_str(&json).expect("a board reads back");
        assert_eq!(back, board);
        assert!(back.check().is_ok());
    }

    #[test]
    fn tier_too_high_fails() {
        let mut board = valid_board();
        board.players[0].tier = MAX_TIER + 1;
        assert!(board.check().is_err());
    }
}
