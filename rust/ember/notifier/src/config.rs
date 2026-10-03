//! The notifier's configuration file.
use std::{
    collections::BTreeMap,
    path::{Path, PathBuf},
};

use ember_protocol::{json, rooms::RoomCreator};
use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

pub(crate) const MAX_BODY: usize = 64 * 1024;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub listen: String,
    /// The bridge origin whose events are accepted (`source`).
    pub bridge_origin: String,
    /// Subscription secrets (`whsec_...`); several during a rotation. A value
    /// `env:NAME` reads the environment variable `NAME`.
    pub secrets: Vec<String>,
    pub database: PathBuf,
    #[serde(default)]
    pub discord: Option<Discord>,
    #[serde(default)]
    pub twitch: Option<Twitch>,
    /// The bridge connection the room commands act through.
    #[serde(default)]
    pub bot: Option<Bot>,
    /// Display names for Ember IDs; others show their short fingerprint.
    #[serde(default)]
    pub names: BTreeMap<String, String>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Discord {
    /// The channel webhook URL, or `env:NAME`. Without one nothing is posted
    /// to a channel; the `/room` command can still be served.
    #[serde(default)]
    pub webhook_url: Option<String>,
    #[serde(default = "default_username")]
    pub username: String,
    /// The `/room` slash command, answered over Discord's HTTP interactions.
    #[serde(default)]
    pub interactions: Option<Interactions>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Interactions {
    pub application_id: String,
    /// The application's public key, as 64 hex characters.
    pub public_key: String,
    #[serde(default = "default_discord_api")]
    pub api_base: String,
    /// A bot token, or `env:NAME`. Only `discord-register` uses it.
    #[serde(default)]
    pub bot_token: Option<String>,
    /// Register the command for one server instead of every server.
    #[serde(default)]
    pub guild_id: Option<String>,
}

fn default_discord_api() -> String {
    "https://discord.com/api/v10".into()
}

fn default_username() -> String {
    "Ember".into()
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Twitch {
    #[serde(default = "default_twitch_api")]
    pub api_base: String,
    pub client_id: String,
    /// A user access token with `user:write:chat`, or `env:NAME`.
    pub token: String,
    pub broadcaster_id: String,
    pub sender_id: String,
    /// The streamer's linked player. With it, `!room` from the broadcaster or
    /// a moderator opens a room for them.
    #[serde(default)]
    pub room_creator: Option<RoomCreator>,
    #[serde(default = "default_eventsub_url")]
    pub eventsub_url: String,
}

fn default_twitch_api() -> String {
    "https://api.twitch.tv".into()
}

fn default_eventsub_url() -> String {
    "wss://eventsub.wss.twitch.tv/ws".into()
}

/// The provider credential of a bridge connection with `rooms` (and
/// `discord_lookup` for the Discord command).
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Bot {
    pub bridge_api: String,
    /// The provider credential, or `env:NAME`.
    pub credential: String,
    /// The build rooms are created for.
    pub build_id: String,
    #[serde(default = "default_capacity")]
    pub capacity: u8,
    /// The name of a room when the command gives none.
    #[serde(default)]
    pub room_name: Option<String>,
}

fn default_capacity() -> u8 {
    8
}

/// Resolves `env:NAME` to the variable's value.
pub(crate) fn resolve(value: &str) -> Result<Zeroizing<String>, String> {
    match value.strip_prefix("env:") {
        Some(name) => std::env::var(name)
            .map(Zeroizing::new)
            .map_err(|_| format!("environment variable {name} is not set")),
        None => Ok(Zeroizing::new(value.to_owned())),
    }
}

impl Config {
    pub fn load(path: &Path) -> Result<Self, String> {
        let bytes = std::fs::read(path)
            .map_err(|error| format!("cannot read {}: {error}", path.display()))?;
        let mut config: Self = json::parse_as(&bytes, MAX_BODY)
            .map_err(|error| format!("invalid configuration: {error}"))?;
        if config.database.is_relative() {
            config.database = path
                .parent()
                .unwrap_or(Path::new("."))
                .join(&config.database);
        }
        Ok(config)
    }
}
