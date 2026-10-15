//! Posting an announcement to Twitch chat.
use serde_json::{Value, json};

use crate::{
    Notifier,
    format::Message,
    sent::{Reset, Sent, bounded_body, classify},
};

const TWITCH_MAX: usize = 500;

impl Notifier {
    pub(crate) async fn twitch(&self, message: &Message) -> Sent {
        self.chat(&format!("{}: {}", message.title, message.text))
            .await
    }

    /// Posts one chat message as the configured sender.
    pub(crate) async fn chat(&self, text: &str) -> Sent {
        let (Some(config), Some(token)) = (&self.0.config.twitch, &self.0.twitch_token) else {
            return Sent::Done;
        };
        let mut text = text.to_owned();
        if text.chars().count() > TWITCH_MAX {
            text = text.chars().take(TWITCH_MAX - 1).collect::<String>() + "…";
        }
        let body = json!({
            "broadcaster_id": config.broadcaster_id,
            "sender_id": config.sender_id,
            "message": text,
        });
        let request = self
            .0
            .http
            .post(format!(
                "{}/helix/chat/messages",
                config.api_base.trim_end_matches('/')
            ))
            .bearer_auth(token.as_str())
            .header("client-id", &config.client_id)
            .header("content-type", "application/json")
            .body(serde_json::to_vec(&body).unwrap_or_default());
        match request.send().await {
            // Twitch answers 200 for a message it then drops (AutoMod, a ban,
            // slow mode), so success is what the answer says.
            Ok(response) if response.status().is_success() => {
                match bounded_body(response, MAX_TWITCH_ANSWER).await {
                    Some(body) => twitch_outcome(&body),
                    None => Sent::Failed("unreadable response".into()),
                }
            }
            other => classify(other, Reset::At("ratelimit-reset"), self.0.clock.now()).await,
        }
    }
}

const MAX_TWITCH_ANSWER: usize = 64 * 1024;

/// Reads `POST /helix/chat/messages`: `{"data":[{"is_sent":..,"drop_reason":..}]}`.
fn twitch_outcome(body: &[u8]) -> Sent {
    let Ok(value) = serde_json::from_slice::<Value>(body) else {
        return Sent::Failed("unexpected response".into());
    };
    let entry = &value["data"][0];
    match entry["is_sent"].as_bool() {
        Some(true) => Sent::Done,
        Some(false) => {
            // The code is Twitch's text: keep it short and plain for the log.
            let code: String = entry["drop_reason"]["code"]
                .as_str()
                .unwrap_or("unknown")
                .chars()
                .filter(|c| c.is_ascii_alphanumeric() || *c == '_')
                .take(64)
                .collect();
            Sent::Failed(format!("dropped: {code}"))
        }
        None => Sent::Failed("unexpected response".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_dropped_twitch_message_is_not_delivered() {
        let sent = br#"{"data":[{"message_id":"m1","is_sent":true,"drop_reason":null}]}"#;
        assert!(matches!(twitch_outcome(sent), Sent::Done));
        let dropped = br#"{"data":[{"message_id":"","is_sent":false,"drop_reason":{"code":"msg_rejected","message":"AutoMod"}}]}"#;
        assert!(
            matches!(twitch_outcome(dropped), Sent::Failed(reason) if reason == "dropped: msg_rejected")
        );
        assert!(matches!(twitch_outcome(b""), Sent::Failed(_)));
    }
}
