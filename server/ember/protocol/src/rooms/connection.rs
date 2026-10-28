//! The room routes of a provider connection: a room it opens for a linked
//! player, and the room as that connection sees it.
use serde::{Deserialize, Serialize};

use crate::{EmberId, Error, Result};

use super::{CreateRoom, RoomSummary};

/// A creator who is not linked on the calling connection.
pub const NOT_LINKED: &str = "not_linked";
/// Why a room a connection created closed: the connection closed it, or it
/// ended on its own (it emptied, its creator never came, or its host stopped).
pub const CLOSED_BY_CONNECTION: &str = "closed_by_connection";
pub const ENDED: &str = "ended";
const MAX_REASON: usize = 256;

/// The page a room link opens. It hands Ember the room, or offers to install
/// Ember first. The room rides in the fragment, which browsers never send.
pub const ROOM_PAGE: &str = "https://embernetplay.link/r#";

/// A room's link. Not a secret: the room is public, and admission is still
/// the ticket's.
pub fn join_url(bridge_id: &str, room_id: &str) -> String {
    format!("{ROOM_PAGE}{bridge_id}/{room_id}")
}

/// The player a connection opens a room for: linked on that connection.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RoomCreator {
    pub participant_id: String,
    pub ember_id: EmberId,
}

/// `POST /v1/rooms` with a provider credential: a room for a linked player,
/// who goes in first and moderates it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ConnectionCreateRoom {
    pub name: String,
    pub capacity: u8,
    pub build_id: String,
    pub creator: RoomCreator,
}

impl ConnectionCreateRoom {
    /// The room part, as a player's request would carry it.
    pub fn room(&self) -> CreateRoom {
        CreateRoom {
            name: self.name.clone(),
            capacity: self.capacity,
            build_id: self.build_id.clone(),
            region: None,
            own_delay: false,
        }
    }

    pub fn check(&self) -> Result<()> {
        if self.creator.participant_id.is_empty() || self.creator.participant_id.len() > 128 {
            return Err(Error::InvalidField("creator"));
        }
        self.room().check()
    }
}

/// Where a connection's room stands: waiting for its creator, open to anyone,
/// or closed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RoomState {
    Waiting,
    Open,
    Closed,
}

/// A room as the connection that created it sees it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ConnectionRoom {
    pub room: RoomSummary,
    pub state: RoomState,
    pub creator_ember_id: EmberId,
    pub join_url: String,
}

/// `GET /v1/rooms` with a provider credential: the connection's rooms that
/// are not closed, newest first.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ConnectionRoomList {
    pub rooms: Vec<ConnectionRoom>,
}

/// `POST /v1/rooms/{room_id}/close`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CloseRoom {
    pub reason: String,
}

impl CloseRoom {
    pub fn check(&self) -> Result<()> {
        if self.reason.is_empty()
            || self.reason.len() > MAX_REASON
            || self.reason.chars().any(char::is_control)
        {
            return Err(Error::InvalidField("reason"));
        }
        Ok(())
    }
}
