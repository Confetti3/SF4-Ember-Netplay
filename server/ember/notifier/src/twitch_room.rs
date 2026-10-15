//! Twitch's `!room`: from the broadcaster or a moderator, it opens a room for
//! the configured creator and posts its link in chat.
use serde::Deserialize;

use crate::{
    Notifier,
    bot::{Opened, room_line},
    sent::Sent,
};

const COMMAND: &str = "!room";

/// The part of a `channel.chat.message` event the command reads.
#[derive(Debug, Deserialize)]
pub(crate) struct ChatEvent {
    pub broadcaster_user_id: String,
    pub message: ChatText,
    #[serde(default)]
    pub badges: Vec<Badge>,
}

#[derive(Debug, Deserialize)]
pub(crate) struct ChatText {
    pub text: String,
}

#[derive(Debug, Deserialize)]
pub(crate) struct Badge {
    pub set_id: String,
}

impl ChatEvent {
    /// Whether the message starts with `!room`.
    pub(crate) fn is_room_command(&self) -> bool {
        self.message
            .text
            .split_whitespace()
            .next()
            .is_some_and(|word| word.eq_ignore_ascii_case(COMMAND))
    }

    /// Whether the sender is the broadcaster or a moderator.
    pub(crate) fn is_from_host(&self) -> bool {
        self.badges
            .iter()
            .any(|badge| matches!(badge.set_id.as_str(), "broadcaster" | "moderator"))
    }
}

impl Notifier {
    /// The Twitch listener runs when there is a creator to open rooms for.
    pub(crate) fn twitch_room_enabled(&self) -> bool {
        self.0.bridge.is_some()
            && self
                .0
                .config
                .twitch
                .as_ref()
                .is_some_and(|twitch| twitch.room_creator.is_some())
    }

    /// Answers a chat message in the configured channel.
    pub(crate) async fn on_chat(&self, event: ChatEvent) {
        let Some(twitch) = &self.0.config.twitch else {
            return;
        };
        let Some(creator) = twitch.room_creator.clone() else {
            return;
        };
        if event.broadcaster_user_id != twitch.broadcaster_id
            || !event.is_room_command()
            || !event.is_from_host()
        {
            return;
        }
        // A creator who already has a room gets that room's link again.
        let text = match self.open_room(creator, None, None).await {
            Opened::Room(room) => format!("Room open: {}", room_line(&room.room.name, &room)),
            Opened::NotLinked => {
                "The streamer's Ember ID is not linked to this bot yet.".to_owned()
            }
            Opened::Failed(text) => text,
        };
        if let Sent::Retry(error, _) | Sent::Failed(error) = self.chat(&text).await {
            eprintln!("ember-notifier: Twitch !room reply not sent: {error}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn event(text: &str, badges: &[&str]) -> ChatEvent {
        serde_json::from_value(serde_json::json!({
            "broadcaster_user_id": "1001",
            "chatter_user_id": "3003",
            "message": { "text": text, "fragments": [] },
            "badges": badges.iter().map(|id| serde_json::json!({ "set_id": id, "id": "1", "info": "" })).collect::<Vec<_>>(),
        }))
        .unwrap()
    }

    #[test]
    fn only_the_room_command_is_read() {
        assert!(event("!room", &[]).is_room_command());
        assert!(event("  !ROOM please", &[]).is_room_command());
        assert!(!event("!roomy", &[]).is_room_command());
        assert!(!event("hello !room", &[]).is_room_command());
        assert!(!event("", &[]).is_room_command());
    }

    #[test]
    fn only_the_broadcaster_and_moderators_may_ask() {
        assert!(event("!room", &["broadcaster"]).is_from_host());
        assert!(event("!room", &["subscriber", "moderator"]).is_from_host());
        assert!(!event("!room", &["subscriber", "vip"]).is_from_host());
        assert!(!event("!room", &[]).is_from_host());
    }
}
