//! Secrets of the services the bridge calls, kept apart from the bridge's own
//! keys (`secrets.rs`) so adding a service never touches those. The file is
//! owner-only like the others and is written by the deployment scripts.
use std::{collections::BTreeMap, path::Path};

use ember_protocol::{json, webhook::Secret};
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
    #[serde(default)]
    discord_bot_token: Option<String>,
    #[serde(default)]
    rooms_supervisor_secret: Option<String>,
    #[serde(default)]
    archive_secret_key: Option<String>,
    /// Platform API keys by connection ID.
    #[serde(default)]
    api_keys: BTreeMap<String, String>,
    /// Result signing secrets (`whsec_...`) by connection ID.
    #[serde(default)]
    result_secrets: BTreeMap<String, String>,
    /// Sign-in sites' client secrets, by site ID.
    #[serde(default)]
    site_client_secrets: BTreeMap<String, String>,
}

#[derive(Default)]
pub struct Secrets {
    /// Discord's client secret. Without it Discord sign-in stays off.
    pub discord_client_secret: Option<Zeroizing<String>>,
    /// The Discord app's bot token. Without it no private match messages
    /// are sent (`delivery::dms`).
    pub discord_bot_token: Option<Zeroizing<String>>,
    /// The bearer secret of the room supervisor's API. Without it public
    /// rooms stay off.
    pub rooms_supervisor_secret: Option<Zeroizing<String>>,
    /// The replay archive's S3 secret key. Without it nothing is stored.
    pub archive_secret_key: Option<Zeroizing<String>>,
    /// A platform's API key, by connection ID. A BluMint connection without
    /// one is served but not sent results.
    pub api_keys: BTreeMap<String, Zeroizing<String>>,
    /// The secret a connection's results are signed with, by connection ID.
    /// A connection with a `results_url` and none is served but not sent
    /// results.
    pub result_secrets: BTreeMap<String, Secret>,
    /// A sign-in site's client secret, by site ID. A site without one is
    /// listed for accounts but not offered for sign-in.
    pub site_client_secrets: BTreeMap<String, Zeroizing<String>>,
}

impl Secrets {
    /// The file `config` names, or none. A file not written yet reads as
    /// empty, so a deployment can name it before any secret is stored.
    pub fn load(config: &Config) -> Result<Self, String> {
        let Some(path) = config
            .integration_secrets
            .as_deref()
            .filter(|path| path.exists())
        else {
            return Ok(Self::default());
        };
        let file = read(path)?;
        Ok(Self {
            discord_client_secret: file
                .discord_client_secret
                .filter(|secret| !secret.is_empty())
                .map(Zeroizing::new),
            discord_bot_token: file
                .discord_bot_token
                .filter(|token| !token.is_empty())
                .map(Zeroizing::new),
            rooms_supervisor_secret: file
                .rooms_supervisor_secret
                .filter(|secret| !secret.is_empty())
                .map(Zeroizing::new),
            archive_secret_key: file
                .archive_secret_key
                .filter(|secret| !secret.is_empty())
                .map(Zeroizing::new),
            site_client_secrets: file
                .site_client_secrets
                .into_iter()
                .filter(|(_, secret)| !secret.is_empty())
                .map(|(site, secret)| (site, Zeroizing::new(secret)))
                .collect(),
            api_keys: file
                .api_keys
                .into_iter()
                .filter(|(_, key)| !key.is_empty())
                .map(|(connection, key)| (connection, Zeroizing::new(key)))
                .collect(),
            result_secrets: file
                .result_secrets
                .into_iter()
                .filter(|(_, secret)| !secret.is_empty())
                .map(|(connection, secret)| {
                    let secret = Zeroizing::new(secret);
                    Secret::parse(&secret)
                        .map(|secret| (connection.clone(), secret))
                        .map_err(|_| {
                            format!("the result secret for {connection} is not a whsec_ secret")
                        })
                })
                .collect::<Result<_, String>>()?,
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
