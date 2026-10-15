//! Posting an announcement to a Discord channel webhook.
use ember_protocol::event::Event;
use serde_json::json;

use crate::{
    Notifier,
    format::{Message, escape_markdown},
    sent::{Reset, Sent, classify},
};

impl Notifier {
    pub(crate) async fn discord(&self, message: &Message, event: &Event) -> Sent {
        let (Some(config), Some(url)) = (&self.0.config.discord, &self.0.discord_url) else {
            return Sent::Done;
        };
        let body = json!({
            "username": config.username,
            // Event text never pings anyone.
            "allowed_mentions": { "parse": [] },
            "embeds": [{
                "title": escape_markdown(&message.title),
                "description": escape_markdown(&message.text),
                "color": message.color,
                "timestamp": event.time,
            }],
        });
        let separator = if url.contains('?') { '&' } else { '?' };
        let request = self
            .0
            .http
            .post(format!("{}{separator}wait=true", url.as_str()))
            .header("content-type", "application/json")
            .body(serde_json::to_vec(&body).unwrap_or_default());
        classify(
            request.send().await,
            Reset::After("x-ratelimit-reset-after"),
            self.0.clock.now(),
        )
        .await
    }
}
