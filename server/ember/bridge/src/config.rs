//! Bridge configuration. Tenants and provider connections are declared here;
//! credentials are issued with `ember-bridge credential` and kept in the
//! database as keyed hashes.
use std::{fs, path::Path, path::PathBuf};

use ember_protocol::encoding::{OriginPolicy, check_origin, is_prefixed_id};
use serde::{Deserialize, Serialize};

pub use self::sites::SignInSite;
use crate::linking::Site;

mod deployed;
pub mod sites;

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
    /// Sign-in sites, defined as data (`sites`), so a player can connect
    /// their account on each to their Ember ID.
    #[serde(default)]
    pub sign_in_sites: Vec<SignInSite>,
    /// An owner-only file with the secrets of the services this bridge calls
    /// (`integrations::Secrets`). Needed when `discord` or a sign-in site is
    /// set.
    #[serde(default)]
    pub integration_secrets: Option<PathBuf>,
    /// Public rooms, listed by this bridge and hosted by a supervisor on this
    /// machine. Off when absent.
    #[serde(default)]
    pub rooms: Option<Rooms>,
    /// Hours a match nobody plays stays open, counted from its creation, within
    /// `MIN_MATCH_EXPIRY_HOURS` to `MAX_MATCH_EXPIRY_HOURS`. Lobby and
    /// tournament sets never expire this way.
    #[serde(default = "default_match_expiry_hours")]
    pub match_expiry_hours: u64,
    pub tenants: Vec<Tenant>,
}

pub const DEFAULT_MATCH_EXPIRY_HOURS: u64 = 24;
pub const MIN_MATCH_EXPIRY_HOURS: u64 = 1;
pub const MAX_MATCH_EXPIRY_HOURS: u64 = 14 * 24;

fn default_match_expiry_hours() -> u64 {
    DEFAULT_MATCH_EXPIRY_HOURS
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
    /// The sign-in sites the connection may find players by
    /// (`POST /v1/players/lookup` `accounts`). Kind `blumint` never may.
    #[serde(default)]
    pub lookup_sites: Vec<String>,
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

    /// Whether the connection may find players by their account on `site`.
    pub fn finds_by(&self, site: &Site) -> bool {
        match site {
            Site::Discord => self.discord_lookup(),
            Site::Configured(id) => self.lookup_sites.contains(id),
        }
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
        let chosen = self.discord_lookup
            || !self.lookup_sites.is_empty()
            || self.disputes.is_some()
            || self.results_url.is_some();
        if self.is_blumint() && chosen {
            return Err(format!(
                "{}: a blumint connection's lookup, disputes and results are fixed by its kind",
                self.id
            ));
        }
        if let Some(unknown) = self
            .lookup_sites
            .iter()
            .find(|id| config.sign_in_site(id).is_none())
        {
            return Err(format!(
                "{}: lookup_sites names {unknown:?}, which is no sign-in site",
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
    /// The platform hears how a set stands while it is played: its status
    /// as each player arrives or leaves, as each game starts and as each game
    /// ends with another to come (`delivery::status`), and its score after
    /// each game that does not end it (`delivery::progress`). BluMint.
    pub reports_progress: bool,
    /// The longest a match lives after it was made, however it is played:
    /// games move its deadline as on any connection, but never past that
    /// (`matches.ends_by`). None when only `match_expiry_hours` applies.
    /// BluMint's is `BLUMINT_MATCH_LIFETIME_SECS`.
    pub max_lifetime_secs: Option<u64>,
    /// The platform fixes who may watch its matches (BluMint), so its
    /// organizer's watch settings are read-only: the routes that change them
    /// refuse (`routes::play::watch`).
    pub fixed_watchers: bool,
}

/// The longest a match on a BluMint connection lives,
/// whatever `match_expiry_hours` says. BluMint excuses a player kept busy by a
/// match it could not close only for a bounded window: this cap, plus its own
/// create retry span and a minute for this bridge's maintenance sweep. A match
/// still open past the cap would keep its players busy after BluMint counts
/// them free to be disqualified.
pub const BLUMINT_MATCH_LIFETIME_SECS: u64 = 24 * 60 * 60;

impl Policy {
    pub fn of(kind: &str, disputes: Disputes) -> Self {
        Self {
            own_api_only: kind == BLUMINT,
            reviews_disputes: disputes == Disputes::Review,
            reports_progress: kind == BLUMINT,
            max_lifetime_secs: (kind == BLUMINT).then_some(BLUMINT_MATCH_LIFETIME_SECS),
            fixed_watchers: kind == BLUMINT,
        }
    }
}

/// The largest configuration file the bridge reads.
pub const MAX_CONFIG_LEN: usize = 64 * 1024;

impl Config {
    pub fn load(path: &Path) -> Result<Self, String> {
        let text =
            fs::read(path).map_err(|error| format!("cannot read {}: {error}", path.display()))?;
        let mut config = Self::parse(&text)?;
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

    fn parse(text: &[u8]) -> Result<Self, String> {
        ember_protocol::json::parse_as(text, MAX_CONFIG_LEN)
            .map_err(|error| format!("invalid configuration: {error}"))
    }

    /// A local development configuration with a mock provider, as
    /// `ember-bridge init` writes it.
    pub fn initial(bridge_id: String) -> Self {
        Self {
            bridge_id,
            display_name: "Local Ember bridge".into(),
            origin: "http://127.0.0.1:8787".into(),
            listen: "127.0.0.1:8787".into(),
            database: "bridge.sqlite3".into(),
            secrets: "bridge-secrets.json".into(),
            allow_loopback_http: true,
            allow_private_webhooks: true,
            mock_browser: true,
            discord: None,
            sign_in_sites: Vec::new(),
            integration_secrets: None,
            rooms: None,
            match_expiry_hours: DEFAULT_MATCH_EXPIRY_HOURS,
            tenants: vec![Tenant {
                id: "local".into(),
                name: "Local tournaments".into(),
                connections: vec![Connection {
                    id: "mock-local".into(),
                    kind: "mock".into(),
                    environment: "local".into(),
                    display_name: "Mock provider".into(),
                    enabled: true,
                    api_base: None,
                    discord_lookup: false,
                    lookup_sites: Vec::new(),
                    disputes: None,
                    results_url: None,
                    rooms: None,
                }],
            }],
        }
    }

    /// How long a match nobody plays stays open, in seconds.
    pub fn match_expiry_secs(&self) -> u64 {
        self.match_expiry_hours * 3600
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
        for (index, site) in self.sign_in_sites.iter().enumerate() {
            site.validate(self.policy())?;
            if self.sign_in_sites[..index]
                .iter()
                .any(|other| other.id == site.id)
            {
                return Err(format!("sign-in site {:?} is defined twice", site.id));
            }
        }
        if !self.sign_in_sites.is_empty() && self.integration_secrets.is_none() {
            return Err("sign_in_sites need integration_secrets for their client secrets".into());
        }
        if let Some(rooms) = &self.rooms {
            if !is_loopback_origin(&rooms.supervisor_url) {
                return Err("rooms.supervisor_url must be a loopback http origin".into());
            }
            if self.integration_secrets.is_none() {
                return Err("rooms needs integration_secrets for the supervisor secret".into());
            }
        }
        if !(MIN_MATCH_EXPIRY_HOURS..=MAX_MATCH_EXPIRY_HOURS).contains(&self.match_expiry_hours) {
            return Err(format!(
                "match_expiry_hours must be {MIN_MATCH_EXPIRY_HOURS} to {MAX_MATCH_EXPIRY_HOURS}"
            ));
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

    pub fn sign_in_site(&self, id: &str) -> Option<&SignInSite> {
        self.sign_in_sites.iter().find(|site| site.id == id)
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
    use serde_json::json;

    use super::*;

    /// Whether a configuration with one connection of `kind` and these extra
    /// settings is accepted.
    fn validated(
        kind: &str,
        extra: serde_json::Value,
        loopback: bool,
        allow_private_webhooks: bool,
    ) -> Result<(), String> {
        let mut connection = json!({
            "id": "bm-partner", "kind": kind, "environment": "staging", "display_name": "BluMint",
        });
        connection
            .as_object_mut()
            .unwrap()
            .extend(extra.as_object().unwrap().clone());
        let config: Config = serde_json::from_value(json!({
            "bridge_id": ember_protocol::encoding::prefixed_id("brg", [7; 16]),
            "display_name": "Test bridge",
            "origin": "https://bridge.example",
            "listen": "127.0.0.1:0",
            "database": "bridge.sqlite3",
            "secrets": "secrets.json",
            "allow_loopback_http": loopback,
            "allow_private_webhooks": allow_private_webhooks,
            "tenants": [{ "id": "bm", "name": "BluMint", "connections": [connection] }],
        }))
        .unwrap();
        config.validate()
    }

    fn with_api_base(kind: &str, api_base: &str, loopback: bool) -> Result<(), String> {
        validated(kind, json!({ "api_base": api_base }), loopback, false)
    }

    fn with_settings(kind: &str, settings: serde_json::Value) -> Result<(), String> {
        validated(kind, settings, false, false)
    }

    /// A direct connection with this `results_url`.
    fn with_results_url(url: &str, allow_private_webhooks: bool) -> Result<(), String> {
        validated(
            "direct",
            json!({ "results_url": url }),
            false,
            allow_private_webhooks,
        )
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

    #[test]
    fn blumints_lookup_disputes_and_results_are_fixed_by_its_kind() {
        assert!(with_settings(BLUMINT, json!({})).is_ok());
        for chosen in [
            json!({ "discord_lookup": true }),
            json!({ "lookup_sites": ["challonge"] }),
            json!({ "disputes": "restart" }),
            json!({ "disputes": "review" }),
            json!({ "results_url": "https://bot.example/results" }),
        ] {
            assert!(with_settings(BLUMINT, chosen.clone()).is_err(), "{chosen}");
        }
        // A mock connection, for local testing, may set all of them.
        let all = json!({
            "discord_lookup": true, "disputes": "restart",
            "results_url": "https://bot.example/results",
            "rooms": { "max_open": 8 },
        });
        for kind in ["direct", "mock"] {
            assert!(with_settings(kind, all.clone()).is_ok(), "{kind}");
        }
    }

    #[test]
    fn a_results_url_follows_the_rules_of_a_webhook_url() {
        assert!(with_results_url("https://bot.example/ember/results", false).is_ok());
        let too_long = format!("https://bot.example/{}", "a".repeat(2100));
        for refused in [
            "http://bot.example/results",
            "https://user:secret@bot.example/results",
            "https://bot.example:8443/results",
            "https://bot.example/results#fragment",
            "https://127.0.0.1/results",
            "https://169.254.169.254/results",
            "not a url",
            too_long.as_str(),
        ] {
            assert!(with_results_url(refused, false).is_err(), "{refused}");
        }
        // Local receivers are for tests only; metadata addresses and
        // credentials stay refused.
        assert!(with_results_url("http://127.0.0.1:9000/results", true).is_ok());
        assert!(with_results_url("https://bot.example:8443/results", true).is_ok());
        assert!(with_results_url("http://169.254.169.254/results", true).is_err());
        assert!(with_results_url("http://user:secret@127.0.0.1:9000/results", true).is_err());
    }

    #[test]
    fn a_connection_looks_up_only_sites_the_bridge_has() {
        let config = |lookup_sites: serde_json::Value| -> Result<(), String> {
            let site: serde_json::Value = serde_json::from_slice(
                &std::fs::read(concat!(
                    env!("CARGO_MANIFEST_DIR"),
                    "/deploy/sites/challonge.json"
                ))
                .unwrap(),
            )
            .unwrap();
            let config: Config = serde_json::from_value(json!({
                "bridge_id": ember_protocol::encoding::prefixed_id("brg", [7; 16]),
                "display_name": "Test bridge", "origin": "https://bridge.example", "listen": "127.0.0.1:0",
                "database": "bridge.sqlite3", "secrets": "secrets.json",
                "integration_secrets": "integrations.json",
                "sign_in_sites": [site],
                "tenants": [{ "id": "t", "name": "T", "connections": [{
                    "id": "bot", "kind": "direct", "environment": "staging", "display_name": "Bot",
                    "lookup_sites": lookup_sites,
                }] }],
            }))
            .unwrap();
            config.validate()
        };
        assert!(config(json!(["challonge"])).is_ok());
        assert!(config(json!(["startgg"])).is_err());
    }

    #[test]
    fn a_connections_open_rooms_are_one_to_sixty_four() {
        let rooms =
            |max_open: u32| with_settings("direct", json!({ "rooms": { "max_open": max_open } }));
        for accepted in [1, 8, 64] {
            assert!(rooms(accepted).is_ok(), "{accepted}");
        }
        for refused in [0, 65] {
            assert!(rooms(refused).is_err(), "{refused}");
        }
    }
}
