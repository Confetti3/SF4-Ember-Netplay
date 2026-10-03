//! Opening a room for a linked player, shared by Discord's `/room` and
//! Twitch's `!room`.
use ember_protocol::{
    Error as ProtocolError,
    rooms::{
        ConnectionCreateRoom, ConnectionRoom, INVALID_NAME, MAX_CAPACITY, MAX_NAME, MIN_CAPACITY,
        NOT_LINKED, ROOM_LIMIT, RoomCreator, UNSUPPORTED_BUILD,
    },
};

use crate::{
    Notifier,
    bridge::{BridgeError, Refusal},
};

const DEFAULT_ROOM_NAME: &str = "Ember room";
const BUSY: &str = "Ember could not be reached. Try again in a moment.";

pub(crate) enum Opened {
    /// A new room, or the open one the creator already had.
    Room(ConnectionRoom),
    /// The creator is not linked on the connection.
    NotLinked,
    /// Why it did not work, in words for the person who asked.
    Failed(String),
}

impl Notifier {
    /// Opens a room for `creator`. A creator who already has a room the
    /// connection opened gets that room back.
    pub(crate) async fn open_room(
        &self,
        creator: RoomCreator,
        name: Option<&str>,
        capacity: Option<u8>,
    ) -> Opened {
        let (Some(bot), Some(client)) = (&self.0.config.bot, &self.0.bridge) else {
            return Opened::Failed("Rooms are not set up here.".into());
        };
        let name = name
            .map(str::trim)
            .filter(|name| !name.is_empty())
            .or(bot.room_name.as_deref())
            .unwrap_or(DEFAULT_ROOM_NAME);
        let command = ConnectionCreateRoom {
            name: name.to_owned(),
            capacity: capacity.unwrap_or(bot.capacity),
            build_id: bot.build_id.clone(),
            creator,
        };
        // The bridge checks the same rules; this saves the round trip.
        if let Err(error) = command.check() {
            return Opened::Failed(match error {
                ProtocolError::InvalidField("name") => name_text(),
                ProtocolError::InvalidField("capacity") => capacity_text(),
                _ => "Rooms are not set up correctly here.".into(),
            });
        }
        match client.create_room(&command).await {
            Ok(room) => Opened::Room(room),
            Err(BridgeError::Refused(refusal)) => match refusal.reason.as_deref() {
                Some(ROOM_LIMIT) => match &refusal.room_id {
                    Some(room_id) => match client.room(room_id).await {
                        Ok(room) => Opened::Room(room),
                        Err(_) => Opened::Failed(LIMIT_TEXT.into()),
                    },
                    None => Opened::Failed(LIMIT_TEXT.into()),
                },
                Some(NOT_LINKED) => Opened::NotLinked,
                Some(UNSUPPORTED_BUILD) => {
                    Opened::Failed("Public rooms are not available for this build yet.".into())
                }
                Some(INVALID_NAME) => Opened::Failed(name_text()),
                _ => Opened::Failed(refused_text(&refusal).into()),
            },
            Err(BridgeError::Unavailable) => Opened::Failed(BUSY.into()),
        }
    }
}

fn name_text() -> String {
    format!("A room name is 1 to {MAX_NAME} characters on one line.")
}

fn capacity_text() -> String {
    format!("A room holds {MIN_CAPACITY} to {MAX_CAPACITY} players.")
}

const LIMIT_TEXT: &str = "You already have a room open, or this service has as many as it may.";

fn refused_text(refusal: &Refusal) -> &'static str {
    if refusal.code.retryable() {
        BUSY
    } else {
        "Ember could not open a room."
    }
}

/// A room's announcement line: its name (already escaped for where it is
/// posted), how full it is and its link.
pub(crate) fn room_line(name: &str, room: &ConnectionRoom) -> String {
    format!(
        "{name} ({}/{}): {}",
        room.room.members, room.room.capacity, room.join_url
    )
}
