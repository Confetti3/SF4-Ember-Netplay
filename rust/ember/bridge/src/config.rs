//! Bridge configuration. Tenants and provider connections are declared here;
//! credentials are issued with `ember-bridge credential` and kept in the
//! database as keyed hashes.
use std::{fs, path::Path, path::PathBuf};

use ember_protocol::encoding::{OriginPolicy, check_origin, is_prefixed_id};
use serde::{Deserialize, Serialize};

/// Provider kinds this build can serve. `direct` is a platform that calls
/// the generic API itself with its provider credential; `mock` also gets the
/// mock login pages when `mock_browser` is set; `blumint` also gets BluMint's
/// partner API (`routes::blumint`), whose contract was read from BluMint's
/// published guide and OpenAPI document (spec 22).
pub const SUPPORTED_KINDS: &[&str] = &["direct", "mock", BLUMINT];
pub const BLUMINT: &str = "blumint";

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub bridge_id: String,
    pub display_name: String,
    /// Public origin, the challenge audience and event `source`.
    pub origin: String,
    pub listen: String,
    pub database: PathBuf,
    pub secrets: PathBuf,
    /// Accept `http://` loopback origins. Local development only.
    #[serde(default)]
    pub allow_loopback_http: bool,
    /// Allow webhook destinations on loopback and private networks. Tests
    /// and local receivers only; never in a deployed bridge.
    #[serde(default)]
    pub allow_private_webhooks: bool,
    /// Serve the mock provider's login and approval pages.
    #[serde(default)]
    pub mock_browser: bool,
    /// Discord sign-in, so a player can connect their Discord account to
    /// their Ember ID. Off when absent.
    #[serde(default)]
    pub discord: Option<Discord>,
    /// An owner-only file with the secrets of the services this bridge calls
    /// (`integrations::Secrets`). Needed when `discord` is set.
    #[serde(default)]
    pub integration_secrets: Option<PathBuf>,
    /// Public rooms, listed by this bridge and hosted by a supervisor on this
    /// machine. Off when absent.
    #[serde(default)]
    pub rooms: Option<Rooms>,
    pub tenants: Vec<Tenant>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Discord {
    /// The Discord application's public client ID. Its redirect list must hold
    /// `<origin>/v1/discord/callback`.
    pub client_id: String,
    /// Discord's API, replaced only by tests.
    #[serde(default = "discord_api")]
    pub api_base: String,
}

fn discord_api() -> String {
    "https://discord.com/api".into()
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Rooms {
    /// The room supervisor's loopback API, such as `http://127.0.0.1:8790`.
    /// Its bearer secret is `rooms_supervisor_secret` in the integration
    /// secrets.
    pub supervisor_url: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Tenant {
    pub id: String,
    pub name: String,
    pub connections: Vec<Connection>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Connection {
    pub id: String,
    pub kind: String,
    /// `local`, `staging` or `production`; each is a separate connection.
    pub environment: String,
    pub display_name: String,
    #[serde(default = "enabled")]
    pub enabled: bool,
    /// The platform API this bridge calls for the connection, when the
    /// kind's default for its environment is not the one (tests).
    #[serde(default)]
    pub api_base: Option<String>,
    /// The connection may find players by Discord user ID
    /// (`POST /v1/players/lookup`). Kind `blumint` always may.
    #[serde(default)]
    pub discord_lookup: bool,
    /// What a disputed match does. Stored with the connection's record the
    /// first time it is seen and never changed. Kind `blumint` restarts.
    #[serde(default)]
    pub disputes: Option<Disputes>,
    /// Where each finished match's result is sent, signed with the
    /// connection's result secret (`integrations::Secrets::result_secrets`).
    #[serde(default)]
    pub results_url: Option<String>,
    /// The connection may open public rooms for its linked players.
    #[serde(default)]
    pub rooms: Option<ConnectionRooms>,
}

fn enabled() -> bool {
    true
}

/// What a match that would wait in `needs_review` does instead, or not.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Disputes {
    /// It waits for an organizer.
    Review,
    /// It is cancelled, and the platform replays it.
    Restart,
}

impl Disputes {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Review => "review",
            Self::Restart => "restart",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "review" => Some(Self::Review),
            "restart" => Some(Self::Restart),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ConnectionRooms {
    /// Rooms open at once, 1 to `MAX_OPEN_ROOMS`.
    pub max_open: u8,
}

pub const MAX_OPEN_ROOMS: u8 = 64;

impl Connection {
    pub fn is_blumint(&self) -> bool {
        self.kind == BLUMINT
    }

    /// Whether the connection may find players by Discord user ID.
    pub fn discord_lookup(&self) -> bool {
        self.is_blumint() || self.discord_lookup
    }

    /// What its disputed matches do; recorded with the connection.
    pub fn disputes(&self) -> Disputes {
        match (self.is_blumint(), self.disputes) {
            (true, _) => Disputes::Restart,
            (false, disputes) => disputes.unwrap_or(Disputes::Review),
        }
    }

    /// Whether finished results are sent to the platform: BluMint's API, or
    /// the connection's `results_url`.
    pub fn sends_results(&self) -> bool {
        self.is_blumint() || self.results_url.is_some()
    }

    /// The settings a platform may ask for (`docs/design/INTEGRATION_PATHS.md`).
    /// BluMint's are fixed by its kind.
    fn validate_settings(&self, config: &Config) -> Result<(), String> {
        let chosen = self.discord_lookup || self.disputes.is_some() || self.results_url.is_some();
        if self.is_blumint() && chosen {
            return Err(format!(
                "{}: a blumint connection's lookup, disputes and results are fixed by its kind",
                self.id
            ));
        }
        if let Some(url) = &self.results_url
            && crate::delivery::webhooks::check_url(url, config.allow_private_webhooks).is_err()
        {
            return Err(format!(
                "{}: results_url must be an https URL a webhook could use",
                self.id
            ));
        }
        if let Some(rooms) = &self.rooms
            && !(1..=MAX_OPEN_ROOMS).contains(&rooms.max_open)
        {
            return Err(format!(
                "{}: rooms.max_open must be 1 to {MAX_OPEN_ROOMS}",
                self.id
            ));
        }
        Ok(())
    }
}

/// What a connection's platform expects of its matches, from the
/// connection's stored record (`routes::policy::of`), so a match keeps its
/// policy after the connection leaves the configuration.
#[derive(Clone, Copy, Debug)]
pub struct Policy {
    /// The platform makes its matches through its own API only (BluMint), so
    /// every match on its connection is one it knows; the generic match,
    /// lobby and tournament routes refuse it.
    pub own_api_only: bool,
    /// A disputed match waits in `needs_review` for an organizer, unless the
    /// connection restarts disputes: it is cancelled instead, which the
    /// platform hears as a restart.
    pub reviews_disputes: bool,
}

impl Policy {
    pub fn of(kind: &str, disputes: Disputes) -> Self {
        Self {
            own_api_only: kind == BLUMINT,
            reviews_disputes: disputes == Disputes::Review,
        }
    }
}

impl Config {
    pub fn load(path: &Path) -> Result<Self, String> {
        let text =
            fs::read(path).map_err(|error| format!("cannot read {}: {error}", path.display()))?;
        let mut config: Self = ember_protocol::json::parse_as(&text, 64 * 1024)
            .map_err(|error| format!("invalid configuration: {error}"))?;
        // Relative paths are relative to the configuration file.
        let base = path.parent().unwrap_or(Path::new("."));
        for file in [&mut config.database, &mut config.secrets]
            .into_iter()
            .chain(config.integration_secrets.as_mut())
        {
            if file.is_relative() {
                *file = base.join(&*file);
            }
        }
        config.validate()?;
        Ok(config)
    }

    pub fn policy(&self) -> OriginPolicy {
        if self.allow_loopback_http {
            OriginPolicy::AllowLoopbackHttp
        } else {
            OriginPolicy::HttpsOnly
        }
    }

    pub fn validate(&self) -> Result<(), String> {
        if !is_prefixed_id(&self.bridge_id, "brg") {
            return Err("bridge_id must be brg_<uuid v4>".into());
        }
        check_origin(&self.origin, self.policy())
            .map_err(|_| "origin must be a bare https origin".to_owned())?;
        if self.display_name.is_empty() || self.display_name.len() > 128 {
            return Err("display_name must be 1 to 128 bytes".into());
        }
        if let Some(discord) = &self.discord {
            if !ember_protocol::discord::is_user_id(&discord.client_id) {
                return Err("discord.client_id must be the application's numeric ID".into());
            }
            if !self.is_api_base(&discord.api_base) {
                return Err("discord.api_base must be an https origin followed by /api".into());
            }
            if self.integration_secrets.is_none() {
                return Err("discord needs integration_secrets for its client secret".into());
            }
        }
        if let Some(rooms) = &self.rooms {
            if !is_loopback_origin(&rooms.supervisor_url) {
                return Err("rooms.supervisor_url must be a loopback http origin".into());
            }
            if self.integration_secrets.is_none() {
                return Err("rooms needs integration_secrets for the supervisor secret".into());
            }
        }
        let mut seen = std::collections::BTreeSet::new();
        for tenant in &self.tenants {
            if !is_slug(&tenant.id) || !seen.insert(tenant.id.clone()) {
                return Err(format!("invalid or duplicate tenant id {:?}", tenant.id));
            }
            for connection in &tenant.connections {
                if !is_slug(&connection.id) || !seen.insert(connection.id.clone()) {
                    return Err(format!(
                        "invalid or duplicate connection id {:?}",
                        connection.id
                    ));
                }
                if !SUPPORTED_KINDS.contains(&connection.kind.as_str()) {
                    return Err(format!("unsupported provider kind {:?}", connection.kind));
                }
                if !matches!(
                    connection.environment.as_str(),
                    "local" | "staging" | "production"
                ) {
                    return Err(format!("invalid environment for {}", connection.id));
                }
                connection.validate_settings(self)?;
                // Only a connection the bridge calls out for has an API.
                match &connection.api_base {
                    Some(_) if connection.kind != BLUMINT => {
                        return Err(format!(
                            "{} has an api_base but its kind calls no API",
                            connection.id
                        ));
                    }
                    Some(base) if !self.is_api_base(base) => {
                        return Err(format!(
                            "{}: api_base must be an https origin followed by /api",
                            connection.id
                        ));
                    }
                    _ => {}
                }
            }
        }
        Ok(())
    }

    /// A service's API: an origin under this bridge's policy (https, or
    /// loopback http for local tests), then `/api`, and nothing else.
    fn is_api_base(&self, base: &str) -> bool {
        base.strip_suffix("/api")
            .is_some_and(|origin| check_origin(origin, self.policy()).is_ok())
    }

    pub fn connection(&self, id: &str) -> Option<(&Tenant, &Connection)> {
        self.tenants.iter().find_map(|tenant| {
            tenant
                .connections
                .iter()
                .find(|connection| connection.id == id)
                .map(|connection| (tenant, connection))
        })
    }
}

/// `http://` plus a loopback host and an optional port, nothing else.
fn is_loopback_origin(text: &str) -> bool {
    let Ok(url) = url::Url::parse(text) else {
        return false;
    };
    let host_is_loopback = match url.host() {
        Some(url::Host::Domain(name)) => name == "localhost",
        Some(url::Host::Ipv4(address)) => address.is_loopback(),
        Some(url::Host::Ipv6(address)) => address.is_loopback(),
        None => false,
    };
    url.scheme() == "http"
        && host_is_loopback
        && url.username().is_empty()
        && url.password().is_none()
        && matches!(url.path(), "" | "/")
        && url.query().is_none()
        && url.fragment().is_none()
}

/// `^[a-z][a-z0-9-]{0,63}$`.
pub fn is_slug(text: &str) -> bool {
    let bytes = text.as_bytes();
    !bytes.is_empty()
        && bytes.len() <= 64
        && bytes[0].is_ascii_lowercase()
        && bytes
            .iter()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || *b == b'-')
}

#[cfg(test)]
mod tests {
    use super::*;

    fn with_api_base(kind: &str, api_base: &str, loopback: bool) -> Result<(), String> {
        let config: Config = serde_json::from_value(serde_json::json!({
            "bridge_id": ember_protocol::encoding::prefixed_id("brg", [7; 16]),
            "display_name": "Test bridge",
            "origin": "https://bridge.example",
            "listen": "127.0.0.1:0",
            "database": "bridge.sqlite3",
            "secrets": "secrets.json",
            "allow_loopback_http": loopback,
            "tenants": [{ "id": "bm", "name": "BluMint", "connections": [{
                "id": "bm-partner", "kind": kind, "environment": "staging",
                "display_name": "BluMint", "api_base": api_base,
            }] }],
        }))
        .unwrap();
        config.validate()
    }

    #[test]
    fn a_connections_api_is_an_https_origin_and_api() {
        assert!(with_api_base(BLUMINT, "https://staging.blumint.io/api", false).is_ok());
        for refused in [
            "http://staging.blumint.io/api",
            "http://127.0.0.1:9000/api",
            "https://staging.blumint.io/api/",
            "https://staging.blumint.io/v2/api",
            "https://user:pass@staging.blumint.io/api",
            "https://staging.blumint.io/api?key=1",
            "https://staging.blumint.io",
        ] {
            assert!(with_api_base(BLUMINT, refused, false).is_err(), "{refused}");
        }
        // Local tests may use loopback http, and nothing else.
        assert!(with_api_base(BLUMINT, "http://127.0.0.1:9000/api", true).is_ok());
        assert!(with_api_base(BLUMINT, "http://staging.blumint.io/api", true).is_err());
        // A kind that calls no API takes none.
        assert!(with_api_base("direct", "https://staging.blumint.io/api", false).is_err());
    }
}
