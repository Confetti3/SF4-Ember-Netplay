//! The connector's configuration file. Secrets are never in it, only the
//! paths of the files that hold them.
use std::path::{Path, PathBuf};

use ember_protocol::{
    encoding::{OriginPolicy, check_origin},
    event::parse_rfc3339,
    json,
};
use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

use crate::{rounds::Rounds, schedule::Window};

const MAX_CONFIG: usize = 64 * 1024;
const DEFAULT_POLL_SECS: u64 = 120;
const DEFAULT_MAX_POLL_SECS: u64 = 600;
const MIN_POLL_SECS: u64 = 30;
/// Tournament and set IDs go into URLs and `external_match_id`. With slugs
/// this long, the longest `external_match_id`
/// (`http:<64>:<64>:<attempt>`) stays well within the
/// bridge's 256 bytes.
const MAX_SLUG: usize = 64;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub bridge: Bridge,
    pub results: Results,
    /// Each set's Ember match, the results received and the request counts.
    pub database: PathBuf,
    /// Seconds between reads of a bracket after a change. A result received
    /// reads it at once.
    #[serde(default = "default_poll_secs")]
    pub poll_secs: u64,
    /// The longest wait between reads: each read that finds nothing new
    /// doubles the wait from `poll_secs` up to this.
    #[serde(default = "default_max_poll_secs")]
    pub max_poll_secs: u64,
    pub tournaments: Vec<Tournament>,
    /// Organizers' own events, read from the bridge's desk connection
    /// (`/v1/desk/brackets`, docs/design/TOURNAMENT_DESK.md), which must be
    /// `bridge.connection_id`. Off when absent.
    #[serde(default)]
    pub desk: Option<Desk>,
    /// Lets a tournament's `api_base` be a loopback `http` origin. Local
    /// tests only: in a deployment each organizer's credential goes over
    /// https to its own site (or service) and nowhere else.
    #[serde(default)]
    pub allow_loopback_http: bool,
}

/// The `direct` bridge connection the matches are created on.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Bridge {
    /// The bridge's origin, such as `https://bridge.embernetplay.link`.
    pub api: String,
    /// A file holding the connection's provider credential (`emk_...`).
    pub credential_file: PathBuf,
    /// The connection's ID: results from any other are refused.
    pub connection_id: String,
    /// `required_build_id` of every match: a label for the build the
    /// players use, not checked against the game.
    pub build_id: String,
}

/// The desk's events: each runs as a configured tournament on its site,
/// with the access token the bridge hands out with it.
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Desk {
    /// Sign-in sites that answer the `http` adapter's contract, by bridge
    /// site ID, with their base URL: an event there runs on that adapter at
    /// `<base>/tournaments/<event>/ember` (the bracket site,
    /// docs/design/BRACKET_SITE.md).
    #[serde(default)]
    pub services: std::collections::BTreeMap<String, String>,
    /// Reads the events from this bracket site instead of the bridge: every
    /// tournament it lists as live, run with its connector token.
    #[serde(default)]
    pub site: Option<DeskSite>,
}

/// A bracket site the desk reads its events from (`GET <base>/ember/tournaments`).
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DeskSite {
    /// The key in `services` that names its base URL.
    pub service: String,
    /// Its connector token.
    pub credential_file: PathBuf,
}

/// Where the bridge sends each finished match (the connection's
/// `results_url`).
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Results {
    pub listen: String,
    #[serde(default = "default_results_path")]
    pub path: String,
    /// A file holding the connection's result secret (`whsec_...`).
    pub secret_file: PathBuf,
    /// The secret it replaces, accepted too while the bridge moves over.
    #[serde(default)]
    pub previous_secret_file: Option<PathBuf>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Tournament {
    /// How the bracket is read: `http`, the only adapter.
    pub adapter: Adapter,
    /// A name for the bracket, which its matches' IDs carry.
    pub tournament: String,
    pub credential: Credential,
    pub rounds: Rounds,
    /// When the event runs, as `YYYY-MM-DDTHH:MM:SSZ`. Outside it the
    /// bracket is not read on a schedule and no match is created; a result
    /// received is still read for and reported.
    #[serde(default)]
    pub active_from: Option<String>,
    #[serde(default)]
    pub active_until: Option<String>,
    /// Mark each set underway on the site when its match is created. Costs
    /// one request a set.
    #[serde(default = "default_mark_underway")]
    pub mark_underway: bool,
    /// The service's https base URL (loopback http for local tests).
    #[serde(default)]
    pub api_base: Option<String>,
}

/// The one adapter, kept as a name so configurations and match IDs read
/// as before.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Adapter {
    /// Ember's bracket site, or a service that answers its contract
    /// (ADAPTERS.md).
    Http,
}

impl Adapter {
    pub fn name(self) -> &'static str {
        "http"
    }
}

/// The organizer's access to the site.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Credential {
    pub kind: CredentialKind,
    pub file: PathBuf,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CredentialKind {
    /// A bearer token.
    Bearer,
}

fn default_poll_secs() -> u64 {
    DEFAULT_POLL_SECS
}

fn default_max_poll_secs() -> u64 {
    DEFAULT_MAX_POLL_SECS
}

fn default_results_path() -> String {
    "/results".into()
}

fn default_mark_underway() -> bool {
    true
}

impl Tournament {
    /// `<site>:<tournament>`, the prefix of every `external_match_id` this
    /// tournament's matches carry.
    pub fn key(&self) -> String {
        format!("{}:{}", self.adapter.name(), self.tournament)
    }

    pub(crate) fn check(&self, allow_loopback_http: bool) -> Result<(), String> {
        let key = self.key();
        if !is_slug(&self.tournament) {
            return Err(format!(
                "{key}: tournament is 1 to 64 letters, digits, _ or -"
            ));
        }
        self.rounds
            .check()
            .map_err(|error| format!("{key}: {error}"))?;
        self.window()?;
        match &self.api_base {
            None => Err(format!("{key}: api_base is the service's base URL")),
            Some(base) if !is_service_base(base, allow_loopback_http) => Err(format!(
                "{key}: api_base is an https URL with no user, query or fragment"
            )),
            Some(_) => Ok(()),
        }
    }

    pub(crate) fn window(&self) -> Result<Window, String> {
        let read = |field: &Option<String>, name: &str| {
            field
                .as_deref()
                .map(|text| {
                    parse_rfc3339(text).ok_or_else(|| {
                        format!(
                            "{}: {name} is a UTC time such as 2026-10-18T18:00:00Z",
                            self.key()
                        )
                    })
                })
                .transpose()
        };
        let window = Window {
            from: read(&self.active_from, "active_from")?,
            until: read(&self.active_until, "active_until")?,
        };
        if let (Some(from), Some(until)) = (window.from, window.until)
            && from >= until
        {
            return Err(format!("{}: active_until is after active_from", self.key()));
        }
        Ok(window)
    }
}

/// Reads a secret file, trimmed. Errors name the file, never its contents.
pub(crate) fn read_secret(path: &Path) -> Result<Zeroizing<String>, String> {
    let text = Zeroizing::new(
        std::fs::read_to_string(path)
            .map_err(|error| format!("cannot read {}: {error}", path.display()))?,
    );
    let trimmed = text.trim();
    if trimmed.is_empty() {
        return Err(format!("{} is empty", path.display()));
    }
    Ok(Zeroizing::new(trimmed.to_owned()))
}

/// An ID or URL slug safe in a URL path and in `external_match_id`: 1 to 64
/// letters, digits, `_` or `-`.
pub(crate) fn is_slug(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= MAX_SLUG
        && text
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
}

/// Whether the organizer's token may go to `base`, their own service: an
/// https origin with an optional path of plain segments, or a loopback http
/// one when tests allow it.
pub(crate) fn is_service_base(base: &str, allow_loopback_http: bool) -> bool {
    let policy = if allow_loopback_http {
        OriginPolicy::AllowLoopbackHttp
    } else {
        OriginPolicy::HttpsOnly
    };
    let after_scheme = base.find("://").map_or(0, |at| at + 3);
    let (origin, path) = base[after_scheme..]
        .find('/')
        .map_or((base, ""), |at| base.split_at(after_scheme + at));
    let plain_path = path.split('/').skip(1).all(|segment| {
        !segment.is_empty()
            && segment != "."
            && segment != ".."
            && segment
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
    });
    check_origin(origin, policy).is_ok() && plain_path
}

impl Config {
    /// Reads and checks the file. Relative paths in it are taken from the
    /// file's own directory.
    pub fn load(path: &Path) -> Result<Self, String> {
        let bytes = std::fs::read(path)
            .map_err(|error| format!("cannot read {}: {error}", path.display()))?;
        let mut config: Self = json::parse_as(&bytes, MAX_CONFIG)
            .map_err(|error| format!("invalid configuration: {error}"))?;
        let base = path.parent().unwrap_or(Path::new("."));
        let paths = [
            &mut config.database,
            &mut config.bridge.credential_file,
            &mut config.results.secret_file,
        ]
        .into_iter()
        .chain(config.results.previous_secret_file.as_mut())
        .chain(
            config
                .desk
                .as_mut()
                .and_then(|desk| desk.site.as_mut())
                .map(|site| &mut site.credential_file),
        )
        .chain(
            config
                .tournaments
                .iter_mut()
                .map(|tournament| &mut tournament.credential.file),
        );
        for path in paths {
            if path.is_relative() {
                *path = base.join(&*path);
            }
        }
        config.check()?;
        Ok(config)
    }

    pub fn check(&self) -> Result<(), String> {
        check_origin(&self.bridge.api, OriginPolicy::AllowLoopbackHttp)
            .map_err(|_| "bridge.api is an https origin (http only on loopback)")?;
        if self.bridge.connection_id.is_empty() {
            return Err("bridge.connection_id is required".into());
        }
        if self.bridge.build_id.is_empty() || self.bridge.build_id.len() > 128 {
            return Err("bridge.build_id is 1 to 128 characters".into());
        }
        // A plain path: axum would read braces as a parameter.
        let path = &self.results.path;
        let plain = path
            .bytes()
            .all(|byte| byte.is_ascii_graphic() && !matches!(byte, b'{' | b'}' | b'?' | b'#'));
        if !path.starts_with('/') || !plain {
            return Err("results.path is a plain path starting with /".into());
        }
        if self.poll_secs < MIN_POLL_SECS {
            return Err(format!("poll_secs is at least {MIN_POLL_SECS}"));
        }
        if self.max_poll_secs < self.poll_secs {
            return Err("max_poll_secs is at least poll_secs".into());
        }
        for (site, base) in self.desk.iter().flat_map(|desk| &desk.services) {
            if !is_slug(site) {
                return Err(format!("desk.services: {site} is a bridge site ID"));
            }
            if !is_service_base(base, self.allow_loopback_http) {
                return Err(format!("desk.services: {site} is an https base URL"));
            }
        }
        if let Some(desk) = &self.desk
            && let Some(site) = &desk.site
            && !desk.services.contains_key(&site.service)
        {
            return Err(format!(
                "desk.site: {} is not in desk.services",
                site.service
            ));
        }
        let mut keys = std::collections::BTreeSet::new();
        for tournament in &self.tournaments {
            tournament.check(self.allow_loopback_http)?;
            if !keys.insert(tournament.key()) {
                return Err(format!("{} is listed twice", tournament.key()));
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rounds::Side;

    #[test]
    fn the_organizers_own_service_is_any_https_base() {
        for base in [
            "https://brackets.example",
            "https://brackets.example/ember/v1",
            "https://brackets.example:8443/api",
        ] {
            assert!(is_service_base(base, false), "{base}");
        }
        for base in [
            "http://brackets.example",
            "https://brackets.example/",
            "https://brackets.example/a//b",
            "https://brackets.example/../b",
            "https://brackets.example/api?x=1",
            "https://brackets.example/#x",
            "https://user@brackets.example",
            "https://brackets example",
            "brackets.example",
            "http://127.0.0.1:9000",
        ] {
            assert!(!is_service_base(base, false), "{base}");
        }
        assert!(is_service_base("http://127.0.0.1:9000/svc", true));
        assert!(!is_service_base("http://10.0.0.5:9000", true));
    }

    #[test]
    fn a_file_reads_with_defaults_and_its_own_directory() {
        let dir = crate::test_dir("config");
        let path = dir.join("brackets.json");
        let text = serde_json::json!({
            "bridge": {
                "api": "https://bridge.example",
                "credential_file": "provider.token",
                "connection_id": "brackets",
                "build_id": "ember-1.4",
            },
            "results": { "listen": "127.0.0.1:8791", "secret_file": "/etc/ember/result.secret" },
            "database": "brackets.sqlite3",
            "tournaments": [{
                "adapter": "http",
                "tournament": "spring-pools",
                "credential": { "kind": "bearer", "file": "service.token" },
                "api_base": "https://brackets.example/ember",
                "rounds": { "games_to_win": 2, "rules": [{ "from_round": 4, "games_to_win": 3 }] },
            }],
        });
        std::fs::write(&path, text.to_string()).unwrap();
        let config = Config::load(&path).unwrap();
        assert_eq!(config.database, dir.join("brackets.sqlite3"));
        assert_eq!(config.bridge.credential_file, dir.join("provider.token"));
        assert_eq!(
            config.tournaments[0].credential.file,
            dir.join("service.token")
        );
        assert!(config.results.secret_file.is_absolute());
        assert_eq!(
            (
                config.poll_secs,
                config.max_poll_secs,
                config.results.path.as_str()
            ),
            (120, 600, "/results")
        );
        assert_eq!(config.tournaments[0].window(), Ok(Window::default()));
        assert_eq!(config.tournaments[0].key(), "http:spring-pools");
        assert_eq!(config.tournaments[0].rounds.rules[0].side, Side::Any);
        // A rule that gave no `from_round` still reads, from round 1.
        let mut old = text.clone();
        old["tournaments"][0]["rounds"] =
            serde_json::json!({ "games_to_win": 2, "rules": [{ "games_to_win": 3 }] });
        std::fs::write(&path, old.to_string()).unwrap();
        let rules = Config::load(&path).unwrap().tournaments[0]
            .rounds
            .rules
            .clone();
        assert_eq!((rules[0].from_round, rules[0].phase.as_deref()), (1, None));

        // A rule's phase is a name.
        for rules in [
            serde_json::json!([{ "phase": "", "games_to_win": 3 }]),
            serde_json::json!([{ "phase": "x".repeat(65), "games_to_win": 3 }]),
        ] {
            let mut changed = text.clone();
            changed["tournaments"][0]["rounds"]["rules"] = rules;
            std::fs::write(&path, changed.to_string()).unwrap();
            assert!(Config::load(&path).is_err());
        }

        // Unknown fields, a bad slug, plain http and the old sites are refused.
        for (pointer, value) in [
            ("/bridge/extra", serde_json::json!(1)),
            ("/tournaments/0/tournament", serde_json::json!("a:b")),
            (
                "/tournaments/0/tournament",
                serde_json::json!("spring pools"),
            ),
            ("/bridge/api", serde_json::json!("http://bridge.example")),
            ("/poll_secs", serde_json::json!(5)),
            ("/results/path", serde_json::json!("/results/{id}")),
            ("/tournaments/0/api_base", serde_json::Value::Null),
            (
                "/tournaments/0/api_base",
                serde_json::json!("http://brackets.example/ember"),
            ),
            (
                "/tournaments/0/api_base",
                serde_json::json!("https://brackets.example/ember?x=1"),
            ),
            (
                "/tournaments/0/api_base",
                serde_json::json!("http://127.0.0.1:9000"),
            ),
            ("/max_poll_secs", serde_json::json!(60)),
            (
                "/tournaments/0/active_from",
                serde_json::json!("2026-10-18T18:00:00+02:00"),
            ),
            (
                "/tournaments/0/active_until",
                serde_json::json!("2026-10-18"),
            ),
            ("/tournaments/0/bridge_site", serde_json::json!("challonge")),
            (
                "/tournaments/0/hold",
                serde_json::json!({ "streamed": true }),
            ),
            ("/tournaments/0/adapter", serde_json::json!("challonge")),
            ("/tournaments/0/adapter", serde_json::json!("startgg")),
            (
                "/tournaments/0/credential",
                serde_json::json!({ "kind": "v1_key", "file": "k" }),
            ),
            ("/monthly_request_budget", serde_json::json!(500)),
        ] {
            let mut changed = text.clone();
            let (parent, field) = pointer.rsplit_once('/').unwrap();
            changed.pointer_mut(parent).unwrap()[field] = value;
            std::fs::write(&path, changed.to_string()).unwrap();
            assert!(Config::load(&path).is_err(), "{pointer}");
        }

        // A window reads in UTC, and ends after it starts.
        let mut windowed = text.clone();
        windowed["tournaments"][0]["active_from"] = serde_json::json!("2026-10-18T18:00:00Z");
        windowed["tournaments"][0]["active_until"] = serde_json::json!("2026-10-18T21:00:00Z");
        std::fs::write(&path, windowed.to_string()).unwrap();
        let window = Config::load(&path).unwrap().tournaments[0]
            .window()
            .unwrap();
        assert_eq!(
            (window.from, window.until),
            (Some(1_792_346_400), Some(1_792_357_200))
        );
        windowed["tournaments"][0]["active_until"] = serde_json::json!("2026-10-18T18:00:00Z");
        std::fs::write(&path, windowed.to_string()).unwrap();
        assert!(Config::load(&path).is_err());
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
