//! Discord's `/room` slash command over HTTP interactions.
//!
//! Discord signs each request with the application's key and gives three
//! seconds to answer. The command is answered at once with a private deferred
//! reply; the bridge calls follow. The reply is edited when they finish, and a
//! room's link is posted to the channel as a followup message.
use std::time::Duration;

use axum::{
    body::Bytes,
    extract::State,
    http::{HeaderMap, StatusCode, header},
    response::{IntoResponse, Response},
};
use ed25519_dalek::{Signature, VerifyingKey};
use ember_protocol::rooms::{ConnectionRoom, MAX_CAPACITY, MAX_NAME, MIN_CAPACITY, RoomCreator};
use reqwest::Method;
use serde::Deserialize;
use serde_json::{Value, json};

use crate::{
    Notifier,
    bot::{BUSY, Opened, room_line},
    bridge::BridgeError,
    config::{Config, MAX_BODY, resolve},
    format::escape_markdown,
    sent::{Reset, Sent, classify, http_client},
};

const COMMAND: &str = "room";
const SIGNATURE: &str = "x-signature-ed25519";
const TIMESTAMP: &str = "x-signature-timestamp";
/// How far a request's timestamp may be from now.
const MAX_AGE_SECS: u64 = 300;
/// How often the deferred reply is edited before giving up, and the pause
/// before the second try, which then doubles. A `Retry-After` beyond
/// `MAX_RETRY_AFTER` seconds ends the attempts.
const EDIT_ATTEMPTS: u32 = 3;
const RETRY_BASE_MS: u64 = 500;
const MAX_RETRY_AFTER: u64 = 60;
const ROOM_OPENED: &str = "Your room is open.";
const EPHEMERAL: u64 = 64;

const PING: u8 = 1;
const APPLICATION_COMMAND: u8 = 2;
const PONG: u8 = 1;
const DEFERRED_MESSAGE: u8 = 5;

#[derive(Deserialize)]
struct Interaction {
    #[serde(rename = "type")]
    kind: u8,
    #[serde(default)]
    token: String,
    data: Option<CommandData>,
    member: Option<Member>,
    user: Option<User>,
}

#[derive(Deserialize)]
struct CommandData {
    name: String,
    #[serde(default)]
    options: Vec<CommandOption>,
}

#[derive(Deserialize)]
struct CommandOption {
    name: String,
    value: Option<Value>,
}

#[derive(Deserialize)]
struct Member {
    user: User,
}

#[derive(Deserialize)]
struct User {
    id: String,
}

/// The application's public key from its hex form.
pub(crate) fn public_key(hex: &str) -> Result<VerifyingKey, String> {
    decode_hex::<32>(hex)
        .and_then(|bytes| VerifyingKey::from_bytes(&bytes).ok())
        .ok_or_else(|| "discord.interactions.public_key is not a 64 character hex key".into())
}

fn decode_hex<const N: usize>(text: &str) -> Option<[u8; N]> {
    if text.len() != N * 2 || !text.is_ascii() {
        return None;
    }
    let mut out = [0u8; N];
    for (byte, pair) in out.iter_mut().zip(text.as_bytes().chunks(2)) {
        *byte = u8::from_str_radix(std::str::from_utf8(pair).ok()?, 16).ok()?;
    }
    Some(out)
}

/// Discord signs the timestamp header followed by the raw body.
fn verify(key: &VerifyingKey, timestamp: &str, body: &[u8], signature: &str) -> bool {
    let Some(bytes) = decode_hex::<64>(signature) else {
        return false;
    };
    let mut message = timestamp.as_bytes().to_vec();
    message.extend_from_slice(body);
    key.verify_strict(&message, &Signature::from_bytes(&bytes))
        .is_ok()
}

fn json_response(body: Value) -> Response {
    (
        [(header::CONTENT_TYPE, "application/json")],
        body.to_string(),
    )
        .into_response()
}

/// `POST /discord/interactions`.
pub(crate) async fn receive(
    State(notifier): State<Notifier>,
    headers: HeaderMap,
    body: Bytes,
) -> Response {
    if body.len() > MAX_BODY {
        return StatusCode::PAYLOAD_TOO_LARGE.into_response();
    }
    let header = |name: &str| headers.get(name).and_then(|value| value.to_str().ok());
    let (Some(key), Some(timestamp), Some(signature)) = (
        notifier.0.interaction_key.as_ref(),
        header(TIMESTAMP),
        header(SIGNATURE),
    ) else {
        return StatusCode::UNAUTHORIZED.into_response();
    };
    let fresh = timestamp
        .parse::<u64>()
        .is_ok_and(|sent| sent.abs_diff(notifier.0.clock.now()) <= MAX_AGE_SECS);
    if !fresh || !verify(key, timestamp, &body, signature) {
        return StatusCode::UNAUTHORIZED.into_response();
    }
    let Ok(interaction) = serde_json::from_slice::<Interaction>(&body) else {
        return StatusCode::BAD_REQUEST.into_response();
    };
    match interaction.kind {
        PING => json_response(json!({ "type": PONG })),
        APPLICATION_COMMAND => notifier.command(interaction).await,
        _ => StatusCode::BAD_REQUEST.into_response(),
    }
}

impl Notifier {
    async fn command(&self, interaction: Interaction) -> Response {
        let valid_token = !interaction.token.is_empty()
            && interaction
                .token
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_');
        let discord_id = interaction
            .member
            .map(|member| member.user.id)
            .or(interaction.user.map(|user| user.id));
        let (Some(data), Some(discord_id), true, true) = (
            interaction.data.filter(|data| data.name == COMMAND),
            discord_id,
            self.0.bridge.is_some(),
            valid_token,
        ) else {
            return StatusCode::BAD_REQUEST.into_response();
        };
        let option = |name: &str| {
            data.options
                .iter()
                .find(|option| option.name == name)
                .and_then(|option| option.value.as_ref())
        };
        let name = option("name").and_then(Value::as_str).map(str::to_owned);
        let capacity = option("capacity")
            .and_then(Value::as_u64)
            .map(|value| u8::try_from(value).unwrap_or(u8::MAX));
        let this = self.clone();
        let token = interaction.token;
        tokio::spawn(async move {
            this.open_for(&token, &discord_id, name, capacity).await;
        });
        // Only the person who asked sees the deferred reply; a room's link is
        // posted separately once there is one.
        json_response(json!({ "type": DEFERRED_MESSAGE, "data": { "flags": EPHEMERAL } }))
    }

    /// Looks the person up and opens their room, then settles the deferred
    /// reply. The bridge is only asked after Discord has its answer, so a slow
    /// bridge cannot cost the interaction.
    async fn open_for(
        &self,
        token: &str,
        discord_id: &str,
        name: Option<String>,
        capacity: Option<u8>,
    ) {
        let Some(client) = self.0.bridge.as_ref() else {
            return;
        };
        let text = match client.lookup(discord_id).await {
            Ok(Some(player)) => {
                let creator = RoomCreator {
                    participant_id: player.participant_id,
                    ember_id: player.ember_id,
                };
                match self.open_room(creator, name.as_deref(), capacity).await {
                    Opened::Room(room) => return self.announce(token, &room).await,
                    Opened::NotLinked => self.not_linked_text().await,
                    Opened::Failed(text) => text,
                }
            }
            Ok(None) => self.not_linked_text().await,
            Err(BridgeError::Refused(_)) => "This bot is not allowed to look up players.".into(),
            Err(BridgeError::Unavailable) => BUSY.into(),
        };
        self.edit_original(token, &text).await;
    }

    /// The room's link goes to the channel; the person who asked keeps a short
    /// private note. When the channel cannot be reached the link goes into
    /// that private reply instead.
    async fn announce(&self, token: &str, room: &ConnectionRoom) {
        let line = format!(
            "Room open: {}",
            room_line(&escape_markdown(&room.room.name), room)
        );
        // The reply must be settled before the followup: one sent while the
        // reply is still deferred fills it in, and is private like it.
        let posted = self.edit_original(token, ROOM_OPENED).await
            && match self.webhook(Method::POST, token, "", &line).await {
                Sent::Done => true,
                Sent::Retry(error, _) | Sent::Failed(error) => {
                    eprintln!("ember-notifier: Discord refused the /{COMMAND} followup: {error}");
                    false
                }
            };
        if !posted {
            self.edit_original(token, &line).await;
        }
    }

    /// Replaces the deferred reply, trying again when Discord is busy or rate
    /// limited. The interaction token outlives these waits by a long way.
    /// Whether the reply now holds `text`.
    async fn edit_original(&self, token: &str, text: &str) -> bool {
        let mut wait = Duration::from_millis(RETRY_BASE_MS);
        for attempt in 1..=EDIT_ATTEMPTS {
            match self
                .webhook(Method::PATCH, token, "/messages/@original", text)
                .await
            {
                Sent::Done => return true,
                Sent::Retry(_, after) if attempt < EDIT_ATTEMPTS && after <= MAX_RETRY_AFTER => {
                    tokio::time::sleep(wait.max(Duration::from_secs(after))).await;
                    wait *= 2;
                }
                Sent::Retry(error, _) | Sent::Failed(error) => {
                    eprintln!("ember-notifier: Discord refused the /{COMMAND} reply: {error}");
                    return false;
                }
            }
        }
        false
    }

    async fn not_linked_text(&self) -> String {
        let start = match self.0.bridge.as_ref() {
            Some(client) => client.start_url().await.ok(),
            None => None,
        };
        match start {
            Some(url) => format!(
                "I cannot find your Ember ID. Connect your Discord account in Ember at {url}, then run /{COMMAND} again."
            ),
            None => "I cannot find your Ember ID. Connect your Discord account in Ember, then run the command again.".into(),
        }
    }

    /// Sends a message through the interaction's webhook: an edit of the
    /// original reply, or a followup. Mentions in the text never ping anyone.
    async fn webhook(&self, method: Method, token: &str, tail: &str, text: &str) -> Sent {
        let Some(interactions) = self
            .0
            .config
            .discord
            .as_ref()
            .and_then(|discord| discord.interactions.as_ref())
        else {
            return Sent::Failed("Discord interactions are not configured".into());
        };
        let url = format!(
            "{}/webhooks/{}/{token}{tail}",
            interactions.api_base.trim_end_matches('/'),
            interactions.application_id
        );
        let body = json!({ "content": text, "allowed_mentions": { "parse": [] } });
        // The URL carries the interaction token, so it is never logged.
        classify(
            self.0.http.request(method, url).json(&body).send().await,
            Reset::After("x-ratelimit-reset-after"),
            self.0.clock.now(),
        )
        .await
    }
}

/// `ember-notifier discord-register`: registers `/room` for the application,
/// or for one server when `guild_id` is set.
pub async fn register_commands(config: &Config) -> Result<(), String> {
    let interactions = config
        .discord
        .as_ref()
        .and_then(|discord| discord.interactions.as_ref())
        .ok_or("discord.interactions is not configured")?;
    let token = interactions
        .bot_token
        .as_deref()
        .ok_or("discord.interactions.bot_token is not configured")
        .map_err(str::to_owned)
        .and_then(resolve)?;
    let _ = rustls::crypto::ring::default_provider().install_default();
    let api = interactions.api_base.trim_end_matches('/');
    let application = &interactions.application_id;
    let url = match &interactions.guild_id {
        Some(guild) => format!("{api}/applications/{application}/guilds/{guild}/commands"),
        None => format!("{api}/applications/{application}/commands"),
    };
    let commands = json!([{
        "name": COMMAND,
        "description": "Open a public Ember room",
        "type": 1,
        "options": [
            { "type": 3, "name": "name", "description": "The room's name", "required": false,
              "max_length": MAX_NAME },
            { "type": 4, "name": "capacity", "description": "How many players it holds",
              "required": false, "min_value": MIN_CAPACITY, "max_value": MAX_CAPACITY },
        ],
    }]);
    let response = http_client()?
        .put(url)
        .header("authorization", format!("Bot {}", token.as_str()))
        .json(&commands)
        .send()
        .await
        .map_err(|_| "Discord could not be reached".to_owned())?;
    if response.status().is_success() {
        Ok(())
    } else {
        Err(format!(
            "Discord refused the registration: status {}",
            response.status().as_u16()
        ))
    }
}

#[cfg(test)]
mod tests {
    use ed25519_dalek::{Signer, SigningKey};

    use super::*;

    fn key() -> (SigningKey, VerifyingKey) {
        let signing = SigningKey::from_bytes(&[7u8; 32]);
        let verifying = signing.verifying_key();
        (signing, verifying)
    }

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|byte| format!("{byte:02x}")).collect()
    }

    #[test]
    fn a_signed_request_verifies_and_a_changed_one_does_not() {
        let (signing, verifying) = key();
        let body = br#"{"type":1}"#;
        let mut message = b"1800000000".to_vec();
        message.extend_from_slice(body);
        let signature = hex(&signing.sign(&message).to_bytes());
        assert!(verify(&verifying, "1800000000", body, &signature));
        assert!(!verify(&verifying, "1800000001", body, &signature));
        assert!(!verify(
            &verifying,
            "1800000000",
            br#"{"type":2}"#,
            &signature
        ));
        assert!(!verify(&verifying, "1800000000", body, "00"));
        assert!(!verify(&verifying, "1800000000", body, &"zz".repeat(64)));
    }

    #[test]
    fn the_public_key_is_read_from_hex() {
        let (_, verifying) = key();
        assert_eq!(public_key(&hex(verifying.as_bytes())).unwrap(), verifying);
        assert!(public_key("abcd").is_err());
        assert!(public_key(&"g".repeat(64)).is_err());
    }
}
