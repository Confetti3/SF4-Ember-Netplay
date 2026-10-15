//! Supervisor configuration, loaded from the JSON file named on the command
//! line, and the timings derived from it.
use std::{collections::BTreeMap, net::SocketAddr, path::Path, time::Duration};

use serde::Deserialize;
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;

pub const MIN_SECRET_BYTES: usize = 32;
pub const MAX_ROOMS_LIMIT: usize = 1024;
/// Ports below this are never handed to a room host.
pub const MIN_PORT: u16 = 1024;

/// A room host build the supervisor can run. Each client release needs room
/// hosts built from the same commit, keyed by its sidecar build hash.
#[derive(Clone, Debug, PartialEq, Eq, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Build {
    /// Path to the `sf4e-room-host` binary.
    pub room_host: String,
    /// Path to the `sf4-net` helper the room host starts.
    pub helper: String,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    #[serde(default = "default_bind")]
    pub bind: String,
    /// File holding the bearer secret shared with the bridge.
    pub secret_file: String,
    #[serde(default = "default_max_rooms")]
    pub max_rooms: usize,
    /// Inclusive UDP port range handed out to room hosts, lowest first.
    #[serde(default = "default_port_range")]
    pub port_range: [u16; 2],
    /// Seconds a room may sit at zero members before it is closed.
    #[serde(default = "default_empty_close_secs")]
    pub empty_close_secs: u64,
    /// Seconds a draining supervisor waits for rooms to end on their own.
    #[serde(default = "default_drain_secs")]
    pub drain_secs: u64,
    #[serde(default)]
    pub builds: BTreeMap<String, Build>,
}

fn default_bind() -> String {
    "127.0.0.1:47830".to_owned()
}

fn default_max_rooms() -> usize {
    8
}

fn default_port_range() -> [u16; 2] {
    [45800, 45899]
}

fn default_empty_close_secs() -> u64 {
    120
}

fn default_drain_secs() -> u64 {
    600
}

impl Config {
    /// Reads and checks the configuration file alone, as a reload does.
    pub fn load(path: &Path) -> Result<Self, String> {
        let text = std::fs::read_to_string(path)
            .map_err(|error| format!("cannot read {}: {error}", path.display()))?;
        let config: Config = serde_json::from_str(&text)
            .map_err(|error| format!("invalid configuration {}: {error}", path.display()))?;
        config.validate()?;
        Ok(config)
    }

    pub fn bind_address(&self) -> Result<SocketAddr, String> {
        let address: SocketAddr = self
            .bind
            .parse()
            .map_err(|_| "bind needs an address and port, such as 127.0.0.1:47830".to_owned())?;
        // The bridge reaches the supervisor over loopback with a shared
        // secret; a public bind would expose room control to the network.
        if !address.ip().is_loopback() {
            return Err("bind must be a loopback address".to_owned());
        }
        Ok(address)
    }

    pub fn validate(&self) -> Result<(), String> {
        self.bind_address()?;
        let [low, high] = self.port_range;
        // Each room holds two ports, so the range must have at least two.
        if low < MIN_PORT || low >= high {
            return Err(format!(
                "port_range must be two different ports from {MIN_PORT} up, lowest first"
            ));
        }
        if !(1..=MAX_ROOMS_LIMIT).contains(&self.max_rooms) {
            return Err(format!("max_rooms must be 1 to {MAX_ROOMS_LIMIT}"));
        }
        if self.empty_close_secs == 0 {
            return Err("empty_close_secs must be at least 1".to_owned());
        }
        for (id, build) in &self.builds {
            if id.is_empty() || id.len() > 128 || !id.bytes().all(|byte| byte.is_ascii_graphic()) {
                return Err("every build id must be 1 to 128 printable characters".to_owned());
            }
            if build.room_host.is_empty() || build.helper.is_empty() {
                return Err(format!("build {id} needs room_host and helper paths"));
            }
        }
        Ok(())
    }
}

/// A validated configuration plus the digest of the shared secret. The
/// secret itself is not kept after start-up.
pub struct Settings {
    pub config: Config,
    secret_digest: [u8; 32],
}

impl std::fmt::Debug for Settings {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Settings")
            .field("config", &self.config)
            .finish_non_exhaustive()
    }
}

impl Settings {
    pub fn new(config: Config, secret: &[u8]) -> Result<Self, String> {
        config.validate()?;
        let secret = secret.trim_ascii();
        if secret.len() < MIN_SECRET_BYTES {
            return Err(format!(
                "the secret must be at least {MIN_SECRET_BYTES} bytes"
            ));
        }
        Ok(Self {
            config,
            secret_digest: Sha256::digest(secret).into(),
        })
    }

    /// Reads the configuration and the secret file it names.
    pub fn load(path: &Path) -> Result<Self, String> {
        let config = Config::load(path)?;
        let secret = std::fs::read(&config.secret_file)
            .map_err(|error| format!("cannot read the secret file: {error}"))?;
        Self::new(config, &secret)
    }

    /// Whether an `Authorization` header value carries the shared secret.
    /// Both sides are hashed first so the comparison takes the same time
    /// whatever the length or content of what was presented.
    pub fn authorized(&self, header: Option<&str>) -> bool {
        let presented = header
            .and_then(|value| value.strip_prefix("Bearer "))
            .unwrap_or("");
        let digest: [u8; 32] = Sha256::digest(presented.as_bytes()).into();
        let matches = bool::from(digest.ct_eq(&self.secret_digest));
        matches && !presented.is_empty()
    }
}

/// Every time the supervisor waits on. Tests shrink these.
#[derive(Clone, Debug)]
pub struct Tuning {
    /// How long a new host has to report `hosted`.
    pub hosted_timeout: Duration,
    /// How long a host has to exit after it is asked to close.
    pub kill_grace: Duration,
    /// How long a room may sit at zero members.
    pub empty_close: Duration,
    /// How long a drain waits for rooms to end before closing them.
    pub drain: Duration,
    /// Skip ports another process already holds on this machine.
    pub probe_ports: bool,
}

impl Tuning {
    pub fn from_config(config: &Config) -> Self {
        Self {
            hosted_timeout: Duration::from_secs(30),
            kill_grace: Duration::from_secs(10),
            empty_close: Duration::from_secs(config.empty_close_secs),
            drain: Duration::from_secs(config.drain_secs),
            probe_ports: true,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config(json: &str) -> Config {
        serde_json::from_str(json).unwrap()
    }

    const SECRET: &[u8] = b"0123456789abcdef0123456789abcdef";

    #[test]
    fn defaults_fill_every_optional_field() {
        let parsed = config(r#"{ "secret_file": "secret" }"#);
        assert_eq!(parsed.bind, "127.0.0.1:47830");
        assert_eq!(parsed.max_rooms, 8);
        assert_eq!(parsed.port_range, [45800, 45899]);
        assert_eq!(parsed.empty_close_secs, 120);
        assert_eq!(parsed.drain_secs, 600);
        assert!(parsed.builds.is_empty());
        assert!(Settings::new(parsed, SECRET).is_ok());
    }

    #[test]
    fn a_public_bind_is_refused() {
        for bind in ["0.0.0.0:47830", "203.0.113.5:47830", "[::]:47830", "nope"] {
            let parsed = config(&format!(r#"{{ "secret_file": "s", "bind": "{bind}" }}"#));
            assert!(Settings::new(parsed, SECRET).is_err(), "{bind}");
        }
        for bind in ["127.0.0.1:1", "[::1]:47830"] {
            let parsed = config(&format!(r#"{{ "secret_file": "s", "bind": "{bind}" }}"#));
            assert!(Settings::new(parsed, SECRET).is_ok(), "{bind}");
        }
    }

    #[test]
    fn a_short_secret_is_refused() {
        let parsed = config(r#"{ "secret_file": "s" }"#);
        assert!(Settings::new(parsed.clone(), b"").is_err());
        assert!(Settings::new(parsed.clone(), b"too short\n").is_err());
        // A trailing newline from `echo` does not count toward the length.
        assert!(Settings::new(parsed.clone(), b"0123456789abcdef0123456789abcde\n").is_err());
        assert!(Settings::new(parsed, b"0123456789abcdef0123456789abcdef\n").is_ok());
    }

    #[test]
    fn the_port_range_and_limits_must_be_sane() {
        for fragment in [
            r#""port_range": [45900, 45800]"#,
            r#""port_range": [80, 90]"#,
            r#""max_rooms": 0"#,
            r#""max_rooms": 5000"#,
            r#""empty_close_secs": 0"#,
            r#""builds": { "": { "room_host": "a", "helper": "b" } }"#,
            r#""builds": { "x": { "room_host": "", "helper": "b" } }"#,
        ] {
            let parsed = config(&format!(r#"{{ "secret_file": "s", {fragment} }}"#));
            assert!(Settings::new(parsed, SECRET).is_err(), "{fragment}");
        }
        // One room needs two ports.
        let one = config(r#"{ "secret_file": "s", "port_range": [45800, 45800] }"#);
        assert!(Settings::new(one, SECRET).is_err());
        let pair = config(r#"{ "secret_file": "s", "port_range": [45800, 45801] }"#);
        assert!(Settings::new(pair, SECRET).is_ok());
    }

    #[test]
    fn unknown_fields_are_rejected() {
        assert!(serde_json::from_str::<Config>(r#"{ "secret_file": "s", "typo": 1 }"#).is_err());
        assert!(serde_json::from_str::<Config>(r#"{ "bind": "127.0.0.1:1" }"#).is_err());
    }

    #[test]
    fn only_the_exact_bearer_secret_is_authorized() {
        let settings = Settings::new(config(r#"{ "secret_file": "s" }"#), SECRET).unwrap();
        let good = format!("Bearer {}", std::str::from_utf8(SECRET).unwrap());
        assert!(settings.authorized(Some(&good)));
        assert!(!settings.authorized(None));
        assert!(!settings.authorized(Some("Bearer ")));
        assert!(!settings.authorized(Some("Bearer short")));
        assert!(!settings.authorized(Some(&good[7..])));
        assert!(!settings.authorized(Some(&format!("{good}x"))));
    }

    #[test]
    fn debug_output_never_shows_the_secret() {
        let settings = Settings::new(config(r#"{ "secret_file": "s" }"#), SECRET).unwrap();
        let text = format!("{settings:?}");
        assert!(!text.contains("0123456789abcdef"));
    }
}
