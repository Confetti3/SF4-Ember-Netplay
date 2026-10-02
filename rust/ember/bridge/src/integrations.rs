//! Secrets of the services the bridge calls, kept apart from the bridge's own
//! keys (`secrets.rs`) so adding a service never touches those. The file is
//! owner-only like the others and is written by the deployment scripts.
use std::{collections::BTreeMap, path::Path};

use ember_protocol::json;
use serde::Deserialize;
use zeroize::Zeroizing;

use crate::Config;

const FORMAT: &str = "ember-bridge-integrations";

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct File {
    format: String,
    version: u8,
    #[serde(default)]
    discord_client_secret: Option<String>,
    /// Platform API keys by connection ID.
    #[serde(default)]
    api_keys: BTreeMap<String, String>,
}

#[derive(Default)]
pub struct Secrets {
    /// Present exactly when the configuration turns Discord sign-in on.
    pub discord_client_secret: Option<Zeroizing<String>>,
    /// A platform's API key, by connection ID. A BluMint connection without
    /// one is served but not sent results.
    pub api_keys: BTreeMap<String, Zeroizing<String>>,
}

impl Secrets {
    /// The secrets `config` needs: none without `integration_secrets`.
    pub fn load(config: &Config) -> Result<Self, String> {
        let Some(path) = &config.integration_secrets else {
            return Ok(Self::default());
        };
        let file = read(path)?;
        let discord_client_secret = file
            .discord_client_secret
            .clone()
            .filter(|_| config.discord.is_some());
        if config.discord.is_some() && discord_client_secret.as_deref().is_none_or(str::is_empty) {
            return Err(
                "discord is configured but the integration secrets have no discord_client_secret"
                    .into(),
            );
        }
        Ok(Self {
            discord_client_secret: discord_client_secret.map(Zeroizing::new),
            api_keys: file
                .api_keys
                .into_iter()
                .filter(|(_, key)| !key.is_empty())
                .map(|(connection, key)| (connection, Zeroizing::new(key)))
                .collect(),
        })
    }
}

fn read(path: &Path) -> Result<File, String> {
    let bytes = Zeroizing::new(
        std::fs::read(path).map_err(|error| format!("cannot read integration secrets: {error}"))?,
    );
    let file: File = json::parse_as(&bytes, 16 * 1024)
        .map_err(|_| "invalid integration secrets file".to_owned())?;
    if file.format != FORMAT || file.version != 1 {
        return Err("unsupported integration secrets file".into());
    }
    Ok(file)
}
