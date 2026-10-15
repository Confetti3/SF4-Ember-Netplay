//! Lobbies: an opt-in queue on a provider connection that plays one
//! first-to-N set after another and rotates its seats when a set ends. This
//! is an extension beyond EMBER-TB-001; each set is an ordinary match.
use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use crate::{
    EmberId, Error, Result,
    encoding::is_prefixed_id,
    matches::{MAX_EXTERNAL_ID, check_metadata, games_to_win_valid},
};

/// Who gives up the seat when a set ends. The players who leave join the
/// back of the queue; nobody else is ever put in it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Rotation {
    /// King of the hill: the loser leaves.
    WinnerStays,
    LoserStays,
    /// Both leave, winner first.
    BothRotate,
}

impl Rotation {
    pub const ALL: [Rotation; 3] = [Self::WinnerStays, Self::LoserStays, Self::BothRotate];

    pub fn as_str(self) -> &'static str {
        match self {
            Self::WinnerStays => "winner_stays",
            Self::LoserStays => "loser_stays",
            Self::BothRotate => "both_rotate",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        Self::ALL
            .into_iter()
            .find(|rotation| rotation.as_str() == text)
    }

    /// Slots (0 or 1) that leave after `winner` wins a set, in the order they
    /// rejoin the queue.
    pub fn leaving(self, winner: u8) -> Vec<u8> {
        let loser = 1 - winner;
        match self {
            Self::WinnerStays => vec![loser],
            Self::LoserStays => vec![winner],
            Self::BothRotate => vec![winner, loser],
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateLobby {
    pub external_lobby_id: String,
    pub game: String,
    pub games_to_win: u8,
    pub rotation: Rotation,
    pub required_build_id: String,
    pub metadata: BTreeMap<String, String>,
}

impl CreateLobby {
    pub fn check(&self) -> Result<()> {
        if self.external_lobby_id.is_empty() || self.external_lobby_id.len() > MAX_EXTERNAL_ID {
            return Err(Error::InvalidField("external_lobby_id"));
        }
        if self.game != "usf4" {
            return Err(Error::InvalidField("game"));
        }
        if !games_to_win_valid(self.games_to_win) {
            return Err(Error::InvalidField("games_to_win"));
        }
        if self.required_build_id.is_empty() || self.required_build_id.len() > 128 {
            return Err(Error::InvalidField("required_build_id"));
        }
        check_metadata(&self.metadata)
    }
}

/// A player the provider adds to a lobby's queue, at their own request.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct JoinLobby {
    pub participant_id: String,
    pub ember_id: EmberId,
}

impl JoinLobby {
    pub fn check(&self) -> Result<()> {
        if !is_prefixed_id(&self.participant_id, "epl") {
            return Err(Error::InvalidField("participant_id"));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rotation_names_and_leavers() {
        for rotation in Rotation::ALL {
            assert_eq!(Rotation::parse(rotation.as_str()), Some(rotation));
            let wire = serde_json::to_value(rotation).unwrap();
            assert_eq!(wire, rotation.as_str());
        }
        assert_eq!(Rotation::parse("king_of_the_hill"), None);
        assert_eq!(Rotation::WinnerStays.leaving(0), vec![1]);
        assert_eq!(Rotation::LoserStays.leaving(1), vec![1]);
        assert_eq!(Rotation::BothRotate.leaving(1), vec![1, 0]);
    }

    #[test]
    fn create_checks_its_fields() {
        let mut command = CreateLobby {
            external_lobby_id: "stream-night".into(),
            game: "usf4".into(),
            games_to_win: 2,
            rotation: Rotation::WinnerStays,
            required_build_id: "sf4e-1.1.0".into(),
            metadata: BTreeMap::new(),
        };
        assert!(command.check().is_ok());
        command.games_to_win = 4;
        assert!(command.check().is_ok());
        command.games_to_win = 11;
        assert!(command.check().is_err());
        command.games_to_win = 3;
        command.game = "sf6".into();
        assert!(command.check().is_err());
    }
}
