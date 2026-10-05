//! Twitch EventSub over WebSocket: one session subscribed to the channel's
//! chat messages, kept alive across Twitch's reconnect requests and drops.
//!
//! The session id comes from `session_welcome` and a subscription must be
//! made within ten seconds of it. A `session_reconnect` names a new URL: its
//! own welcome arrives with the subscriptions already carried over, and the
//! old connection is read until then and dropped only after it. Silence for longer than the
//! welcome's keepalive timeout, a close or any error drops the session; the
//! next one subscribes again after a backoff.
use std::{
    collections::VecDeque,
    time::{Duration, Instant},
};

use futures_util::StreamExt;
use serde::Deserialize;
use serde_json::{Value, json};
use tokio::net::TcpStream;
use tokio_websockets::{ClientBuilder, Limits, MaybeTlsStream, WebSocketStream};

use crate::{Notifier, twitch_room::ChatEvent};

type Socket = WebSocketStream<MaybeTlsStream<TcpStream>>;

const SUBSCRIPTION_TYPE: &str = "channel.chat.message";
/// Twitch's own limit is ten seconds from the welcome to the subscription.
const WELCOME_WAIT: Duration = Duration::from_secs(10);
/// Time allowed beyond the keepalive timeout the welcome names.
const KEEPALIVE_GRACE: Duration = Duration::from_secs(2);
const MAX_FRAME: usize = 1024 * 1024;
const FIRST_RETRY_SECS: u64 = 1;
const LAST_RETRY_SECS: u64 = 60;
/// Message IDs remembered to drop the repeats Twitch may send.
const SEEN: usize = 128;

#[derive(Deserialize)]
struct Frame {
    metadata: Metadata,
    #[serde(default)]
    payload: Value,
}

#[derive(Deserialize)]
struct Metadata {
    message_id: String,
    message_type: String,
    #[serde(default)]
    subscription_type: Option<String>,
}

struct Welcome {
    session_id: String,
    keepalive: Duration,
}

#[derive(Default)]
struct Seen(VecDeque<String>);

impl Seen {
    fn first_time(&mut self, id: &str) -> bool {
        if self.0.iter().any(|seen| seen == id) {
            return false;
        }
        if self.0.len() == SEEN {
            self.0.pop_front();
        }
        self.0.push_back(id.to_owned());
        true
    }
}

impl Notifier {
    /// Keeps a session open for as long as the notifier runs.
    pub(crate) async fn eventsub(self) {
        let Some(url) = self
            .0
            .config
            .twitch
            .as_ref()
            .map(|twitch| twitch.eventsub_url.clone())
        else {
            return;
        };
        let mut seen = Seen::default();
        let mut delay = FIRST_RETRY_SECS;
        loop {
            let reason = self.session(&url, &mut delay, &mut seen).await;
            eprintln!("ember-notifier: Twitch EventSub: {reason}; reconnecting in {delay} s");
            tokio::time::sleep(Duration::from_secs(delay)).await;
            delay = (delay * 2).min(LAST_RETRY_SECS);
        }
    }

    /// One session, until it ends. The answer says why. A session that got as
    /// far as subscribing resets the backoff.
    async fn session(&self, url: &str, delay: &mut u64, seen: &mut Seen) -> String {
        let (mut socket, welcome) = match open(url).await {
            Ok(opened) => opened,
            Err(reason) => return reason,
        };
        if let Err(reason) = self.subscribe(&welcome.session_id).await {
            return reason;
        }
        *delay = FIRST_RETRY_SECS;
        let mut keepalive = welcome.keepalive;
        loop {
            let frame = match next_frame(&mut socket, keepalive + KEEPALIVE_GRACE).await {
                Ok(frame) => frame,
                Err(reason) => return reason,
            };
            match frame.metadata.message_type.as_str() {
                "notification" => self.notification(&frame, seen),
                "session_reconnect" => {
                    let Some(next) = frame.payload["session"]["reconnect_url"].as_str() else {
                        return "reconnect without a URL".into();
                    };
                    match self.hand_off(&mut socket, keepalive, next, seen).await {
                        Ok((opened, welcome)) => {
                            // The old connection closes as it is replaced.
                            socket = opened;
                            keepalive = welcome.keepalive;
                        }
                        Err(reason) => return reason,
                    }
                }
                "revocation" => return "the subscription was revoked".into(),
                // Keepalives only reset the timer; later message types are
                // not ones this listener needs.
                _ => {}
            }
        }
    }

    /// Opens the replacement connection named by a `session_reconnect`.
    /// Twitch keeps delivering on the old one until the replacement has
    /// welcomed, so it is read, with the same repeat check, in the meantime.
    async fn hand_off(
        &self,
        old: &mut Socket,
        keepalive: Duration,
        next: &str,
        seen: &mut Seen,
    ) -> Result<(Socket, Welcome), String> {
        let opening = open(next);
        tokio::pin!(opening);
        loop {
            tokio::select! {
                opened = &mut opening => return opened,
                frame = next_frame(old, keepalive + KEEPALIVE_GRACE) => match frame {
                    Ok(frame) => match frame.metadata.message_type.as_str() {
                        "notification" => self.notification(&frame, seen),
                        "revocation" => return Err("the subscription was revoked".into()),
                        _ => {}
                    },
                    // The old connection ending early only means nothing more
                    // arrives on it; the replacement is still coming.
                    Err(_) => return opening.await,
                },
            }
        }
    }

    /// `POST /eventsub/subscriptions` for the broadcaster's chat, read as the
    /// sender whose token this is.
    async fn subscribe(&self, session_id: &str) -> Result<(), String> {
        let (Some(twitch), Some(token)) = (&self.0.config.twitch, &self.0.twitch_token) else {
            return Err("Twitch is not configured".into());
        };
        let body = json!({
            "type": SUBSCRIPTION_TYPE,
            "version": "1",
            "condition": {
                "broadcaster_user_id": twitch.broadcaster_id,
                "user_id": twitch.sender_id,
            },
            "transport": { "method": "websocket", "session_id": session_id },
        });
        let response = self
            .0
            .http
            .post(format!(
                "{}/helix/eventsub/subscriptions",
                twitch.api_base.trim_end_matches('/')
            ))
            .bearer_auth(token.as_str())
            .header("client-id", &twitch.client_id)
            .json(&body)
            .send()
            .await
            .map_err(|_| "the subscription request was not sent".to_owned())?;
        if response.status().is_success() {
            Ok(())
        } else {
            Err(format!(
                "Twitch refused the subscription: status {}",
                response.status().as_u16()
            ))
        }
    }

    fn notification(&self, frame: &Frame, seen: &mut Seen) {
        if frame.metadata.subscription_type.as_deref() != Some(SUBSCRIPTION_TYPE)
            || !seen.first_time(&frame.metadata.message_id)
        {
            return;
        }
        let Ok(event) = serde_json::from_value::<ChatEvent>(frame.payload["event"].clone()) else {
            return;
        };
        // Creating a room takes a network round trip; the socket keeps reading.
        let this = self.clone();
        tokio::spawn(async move { this.on_chat(event).await });
    }
}

/// Connects and waits for the welcome.
async fn open(url: &str) -> Result<(Socket, Welcome), String> {
    let builder = ClientBuilder::new()
        .uri(url)
        .map_err(|_| "the EventSub URL is not valid".to_owned())?
        .limits(Limits::default().max_payload_len(Some(MAX_FRAME)));
    let (mut socket, _) = tokio::time::timeout(WELCOME_WAIT, builder.connect())
        .await
        .map_err(|_| "connecting timed out".to_owned())?
        .map_err(|error| format!("cannot connect: {error}"))?;
    loop {
        let frame = next_frame(&mut socket, WELCOME_WAIT).await?;
        if frame.metadata.message_type != "session_welcome" {
            continue;
        }
        let session = &frame.payload["session"];
        return match (
            session["id"].as_str(),
            session["keepalive_timeout_seconds"].as_u64(),
        ) {
            (Some(id), Some(seconds)) => Ok((
                socket,
                Welcome {
                    session_id: id.to_owned(),
                    keepalive: Duration::from_secs(seconds),
                },
            )),
            _ => Err("the welcome had no session".into()),
        };
    }
}

/// The next text frame, within `wait`. Pings are answered by the socket and
/// do not count as a message.
async fn next_frame(socket: &mut Socket, wait: Duration) -> Result<Frame, String> {
    let deadline = tokio::time::Instant::from_std(Instant::now() + wait);
    loop {
        let message = tokio::time::timeout_at(deadline, socket.next())
            .await
            .map_err(|_| "no message within the keepalive timeout".to_owned())?;
        let message = match message {
            None => return Err("the connection closed".into()),
            Some(Err(error)) => return Err(format!("connection error: {error}")),
            Some(Ok(message)) => message,
        };
        if message.is_close() {
            return Err(match message.as_close() {
                Some((code, _)) => format!("closed by Twitch ({})", u16::from(code)),
                None => "closed by Twitch".into(),
            });
        }
        if let Some(text) = message.as_text() {
            return serde_json::from_str(text).map_err(|_| "an unreadable message".to_owned());
        }
    }
}
