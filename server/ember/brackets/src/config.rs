//! The connector's configuration file. Secrets are never in it, only the
//! paths of the files that hold them.
use std::path::{Path, PathBuf};

use ember_protocol::{
    encoding::{OriginPolicy, check_origin},
    event::parse_rfc3339,
    json,
    sites::is_site_id,
};
use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

use crate::{
    rounds::{Hold, Rounds},
    schedule::Window,
};

const MAX_CONFIG: usize = 64 * 1024;
/// Challonge's free tier: 500 requests a month per developer app.
const DEFAULT_BUDGET: u32 = 500;
const DEFAULT_POLL_SECS: u64 = 120;
const DEFAULT_MAX_POLL_SECS: u64 = 600;
const MIN_POLL_SECS: u64 = 30;
/// Tournament and set IDs go into URLs and `external_match_id`. With slugs
/// this long, the longest `external_match_id`
/// (`startgg:tournament/<64>/event/<64>:<64>:<attempt>`) stays within the
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
    /// Requests each Challonge credential may make in a calendar month
    /// (UTC); start.gg keeps to its own rate (`startgg::RATE`) and an `http`
    /// service to its own limits. At 80% the connector warns; at 100% it
    /// stops reading brackets but still reports results.
    #[serde(default = "default_budget")]
    pub monthly_request_budget: u32,
    pub tournaments: Vec<Tournament>,
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
    /// The site the bracket is run on.
    pub adapter: Adapter,
    /// The bridge sign-in site its players' accounts are under; the
    /// adapter's name when absent. The http adapter's service names each
    /// account's site itself.
    #[serde(default)]
    pub bridge_site: Option<String>,
    /// Challonge: the tournament's ID or URL slug. start.gg: the event's
    /// slug, `tournament/<tournament>/event/<event>`. http: a name for the
    /// bracket, which its matches' IDs carry.
    pub tournament: String,
    pub credential: Credential,
    pub rounds: Rounds,
    /// Sets the connector leaves to the organizer for now. Off by default.
    #[serde(default)]
    pub hold: Hold,
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
    /// The site's API origin, when not its public one: an https origin on
    /// the site's own domain, or loopback http for local tests. For the
    /// http adapter, required: the service's https base URL.
    #[serde(default)]
    pub api_base: Option<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Adapter {
    Challonge,
    Startgg,
    /// The organizer's own service, through the contract in ADAPTERS.md.
    Http,
}

impl Adapter {
    pub fn name(self) -> &'static str {
        match self {
            Self::Challonge => "challonge",
            Self::Startgg => "startgg",
            Self::Http => "http",
        }
    }

    /// The domain the organizer's credential may be sent to; any the
    /// organizer configures for their own service.
    fn domain(self) -> Option<&'static str> {
        match self {
            Self::Challonge => Some("challonge.com"),
            Self::Startgg => Some("start.gg"),
            Self::Http => None,
        }
    }

    /// Whether `tournament` names a bracket on this site in a form that is
    /// safe in its requests and in `external_match_id`.
    fn names(self, tournament: &str) -> bool {
        match self {
            Self::Challonge | Self::Http => is_slug(tournament),
            Self::Startgg => {
                let parts: Vec<&str> = tournament.split('/').collect();
                matches!(parts.as_slice(), ["tournament", name, "event", event] if is_slug(name) && is_slug(event))
            }
        }
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
    /// Challonge's v1 API key, which v2.1 accepts.
    V1Key,
    /// An OAuth access token.
    Bearer,
}

fn default_poll_secs() -> u64 {
    DEFAULT_POLL_SECS
}

fn default_max_poll_secs() -> u64 {
    DEFAULT_MAX_POLL_SECS
}

fn default_budget() -> u32 {
    DEFAULT_BUDGET
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

    /// The bridge sign-in site its players' accounts are under.
    pub fn bridge_site(&self) -> &str {
        self.bridge_site
            .as_deref()
            .unwrap_or_else(|| self.adapter.name())
    }

    fn check(&self, allow_loopback_http: bool) -> Result<(), String> {
        let key = self.key();
        if !self.adapter.names(&self.tournament) {
            return Err(match self.adapter {
                Adapter::Challonge => format!("{key}: tournament is a Challonge ID or URL slug"),
                Adapter::Startgg => {
                    format!("{key}: tournament is an event slug, tournament/<name>/event/<name>")
                }
                Adapter::Http => format!("{key}: tournament is 1 to 64 letters, digits, _ or -"),
            });
        }
        match (self.adapter, &self.bridge_site) {
            (Adapter::Http, Some(_)) => {
                return Err(format!(
                    "{key}: the service names each account's site; bridge_site is not used"
                ));
            }
            _ if !is_site_id(self.bridge_site()) => {
                return Err(format!(
                    "{key}: bridge_site is 1 to 32 lowercase letters, digits or -"
                ));
            }
            _ => {}
        }
        if self.adapter != Adapter::Challonge && self.credential.kind != CredentialKind::Bearer {
            return Err(format!("{key}: its token is a bearer credential"));
        }
        if self.hold.streamed && self.adapter != Adapter::Startgg {
            return Err(format!(
                "{key}: hold.streamed is for start.gg, the only site that says which sets are on a stream; an http service holds a set by not listing it open"
            ));
        }
        self.rounds
            .check()
            .map_err(|error| format!("{key}: {error}"))?;
        self.window()?;
        match (self.adapter.domain(), &self.api_base) {
            (Some(domain), Some(origin))
                if !is_site_origin(origin, domain, allow_loopback_http) =>
            {
                Err(format!("{key}: api_base is an https origin on {domain}"))
            }
            (None, None) => Err(format!("{key}: api_base is the service's base URL")),
            (None, Some(base)) if !is_service_base(base, allow_loopback_http) => Err(format!(
                "{key}: api_base is an https URL with no user, query or fragment"
            )),
            _ => Ok(()),
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

/// Whether the organizer's credential for a site on `domain` may go to
/// `origin`: an https origin on `domain` or one of its subdomains, with no
/// port, path or user info, or a loopback http origin when tests allow it.
/// The bridge holds its own sign-in sites to the same rule.
pub(crate) fn is_site_origin(origin: &str, domain: &str, allow_loopback_http: bool) -> bool {
    let policy = if allow_loopback_http {
        OriginPolicy::AllowLoopbackHttp
    } else {
        OriginPolicy::HttpsOnly
    };
    let on_domain = origin.strip_prefix("https://").is_some_and(|host| {
        host == domain
            || host
                .strip_suffix(domain)
                .is_some_and(|sub| sub.ends_with('.'))
    });
    check_origin(origin, policy).is_ok() && (on_domain || origin.starts_with("http://"))
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
        if self.monthly_request_budget == 0 {
            return Err("monthly_request_budget is at least 1".into());
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
    fn the_organizers_credential_goes_to_challonge_only() {
        for origin in [
            "https://api.challonge.com",
            "https://challonge.com",
            "https://staging.api.challonge.com",
        ] {
            assert!(is_site_origin(origin, "challonge.com", false), "{origin}");
        }
        for origin in [
            "http://api.challonge.com",
            "https://evilchallonge.com",
            "https://challonge.com.evil.example",
            "https://api.challonge.co",
            "https://api.challonge.com:8443",
            "https://user@api.challonge.com",
            "https://api.challonge.com/v2.1",
            "https://api.challonge.com/",
            "http://127.0.0.1:9000",
            "",
        ] {
            assert!(!is_site_origin(origin, "challonge.com", false), "{origin}");
        }
        // Loopback http is for tests, and only loopback.
        assert!(is_site_origin(
            "http://127.0.0.1:9000",
            "challonge.com",
            true
        ));
        assert!(is_site_origin(
            "http://localhost:9000",
            "challonge.com",
            true
        ));
        assert!(!is_site_origin(
            "http://api.challonge.com",
            "challonge.com",
            true
        ));
        assert!(!is_site_origin(
            "http://10.0.0.5:9000",
            "challonge.com",
            true
        ));
        // start.gg's tokens go to start.gg the same way, and only there.
        assert!(is_site_origin("https://api.start.gg", "start.gg", false));
        for origin in [
            "https://api.challonge.com",
            "https://evilstart.gg",
            "http://api.start.gg",
        ] {
            assert!(!is_site_origin(origin, "start.gg", false), "{origin}");
        }
    }

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
                "adapter": "challonge",
                "tournament": "ember_weekly_12",
                "credential": { "kind": "v1_key", "file": "challonge.key" },
                "rounds": { "games_to_win": 2, "rules": [{ "from_round": 4, "games_to_win": 3 }] },
            }],
        });
        std::fs::write(&path, text.to_string()).unwrap();
        let config = Config::load(&path).unwrap();
        assert_eq!(config.database, dir.join("brackets.sqlite3"));
        assert_eq!(config.bridge.credential_file, dir.join("provider.token"));
        assert_eq!(
            config.tournaments[0].credential.file,
            dir.join("challonge.key")
        );
        assert!(config.results.secret_file.is_absolute());
        assert_eq!(
            (
                config.poll_secs,
                config.max_poll_secs,
                config.monthly_request_budget,
                config.results.path.as_str()
            ),
            (120, 600, 500, "/results")
        );
        assert_eq!(config.tournaments[0].window(), Ok(Window::default()));
        assert_eq!(config.tournaments[0].key(), "challonge:ember_weekly_12");
        assert_eq!(config.tournaments[0].bridge_site(), "challonge");
        assert_eq!(config.tournaments[0].rounds.rules[0].side, Side::Any);
        // The first config this connector read had no `hold`, and a rule
        // gave its `from_round`: both still read, the rule from round 1.
        assert_eq!(config.tournaments[0].hold, Hold::default());
        let mut old = text.clone();
        old["tournaments"][0]["rounds"] =
            serde_json::json!({ "games_to_win": 2, "rules": [{ "games_to_win": 3 }] });
        std::fs::write(&path, old.to_string()).unwrap();
        let rules = Config::load(&path).unwrap().tournaments[0]
            .rounds
            .rules
            .clone();
        assert_eq!((rules[0].from_round, rules[0].phase.as_deref()), (1, None));

        // A rule's phase is a name, and holding streamed sets is off or on.
        for (rules, hold) in [
            (
                serde_json::json!([{ "phase": "", "games_to_win": 3 }]),
                None,
            ),
            (
                serde_json::json!([{ "phase": "x".repeat(65), "games_to_win": 3 }]),
                None,
            ),
            (
                serde_json::json!([]),
                Some(serde_json::json!({ "later": true })),
            ),
        ] {
            let mut changed = text.clone();
            changed["tournaments"][0]["rounds"]["rules"] = rules;
            if let Some(hold) = hold {
                changed["tournaments"][0]["hold"] = hold;
            }
            std::fs::write(&path, changed.to_string()).unwrap();
            assert!(Config::load(&path).is_err());
        }

        // Unknown fields, a slug with a colon and plain http are refused.
        for (pointer, value) in [
            ("/bridge/extra", serde_json::json!(1)),
            ("/tournaments/0/tournament", serde_json::json!("a:b")),
            ("/bridge/api", serde_json::json!("http://bridge.example")),
            ("/poll_secs", serde_json::json!(5)),
            ("/results/path", serde_json::json!("/results/{id}")),
            (
                "/tournaments/0/api_base",
                serde_json::json!("http://api.challonge.com"),
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
            ("/tournaments/0/bridge_site", serde_json::json!("Challonge")),
            ("/tournaments/0/bridge_site", serde_json::json!("")),
            ("/tournaments/0/adapter", serde_json::json!("smashgg")),
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

        // A start.gg event: its slug, a bearer token, an origin on start.gg,
        // and a bridge site of the operator's naming.
        let mut startgg = text.clone();
        startgg["tournaments"][0] = serde_json::json!({
            "adapter": "startgg",
            "bridge_site": "startgg-staging",
            "tournament": "tournament/ember-weekly-12/event/ultra-street-fighter-iv",
            "credential": { "kind": "bearer", "file": "startgg.token" },
            "api_base": "https://api.start.gg",
            "rounds": { "games_to_win": 2 },
        });
        std::fs::write(&path, startgg.to_string()).unwrap();
        let config = Config::load(&path).unwrap();
        assert_eq!(config.tournaments[0].bridge_site(), "startgg-staging");
        assert_eq!(
            config.tournaments[0].key(),
            "startgg:tournament/ember-weekly-12/event/ultra-street-fighter-iv"
        );
        // Only start.gg says which sets are on a stream.
        let mut held = startgg.clone();
        held["tournaments"][0]["hold"] = serde_json::json!({ "streamed": true });
        std::fs::write(&path, held.to_string()).unwrap();
        assert!(Config::load(&path).unwrap().tournaments[0].hold.streamed);
        let mut challonge = text.clone();
        challonge["tournaments"][0]["hold"] = serde_json::json!({ "streamed": true });
        std::fs::write(&path, challonge.to_string()).unwrap();
        let refused = Config::load(&path).unwrap_err();
        assert!(
            refused.contains("hold.streamed is for start.gg"),
            "{refused}"
        );
        for (field, value) in [
            ("tournament", serde_json::json!("ember-weekly-12")),
            ("tournament", serde_json::json!("tournament/a/event/b/c")),
            ("tournament", serde_json::json!("tournament/a b/event/c")),
            (
                "credential",
                serde_json::json!({ "kind": "v1_key", "file": "k" }),
            ),
            ("api_base", serde_json::json!("https://api.challonge.com")),
            ("api_base", serde_json::json!("http://api.start.gg")),
        ] {
            let mut changed = startgg.clone();
            changed["tournaments"][0][field] = value;
            std::fs::write(&path, changed.to_string()).unwrap();
            assert!(
                Config::load(&path).is_err(),
                "{field}: {}",
                changed["tournaments"][0][field]
            );
        }

        // The organizer's own service: its https base URL is required, it
        // names the accounts' sites itself, and its token is a bearer one.
        let mut service = text.clone();
        service["tournaments"][0] = serde_json::json!({
            "adapter": "http",
            "tournament": "spring-pools",
            "credential": { "kind": "bearer", "file": "service.token" },
            "api_base": "https://brackets.example/ember",
            "rounds": { "games_to_win": 2 },
        });
        std::fs::write(&path, service.to_string()).unwrap();
        assert_eq!(
            Config::load(&path).unwrap().tournaments[0].key(),
            "http:spring-pools"
        );
        for (field, value) in [
            ("api_base", serde_json::Value::Null),
            (
                "api_base",
                serde_json::json!("http://brackets.example/ember"),
            ),
            (
                "api_base",
                serde_json::json!("https://brackets.example/ember?x=1"),
            ),
            ("bridge_site", serde_json::json!("challonge")),
            (
                "credential",
                serde_json::json!({ "kind": "v1_key", "file": "k" }),
            ),
            ("tournament", serde_json::json!("spring pools")),
        ] {
            let mut changed = service.clone();
            changed["tournaments"][0][field] = value;
            std::fs::write(&path, changed.to_string()).unwrap();
            assert!(
                Config::load(&path).is_err(),
                "{field}: {}",
                changed["tournaments"][0][field]
            );
        }
        let mut held_service = service.clone();
        held_service["tournaments"][0]["hold"] = serde_json::json!({ "streamed": true });
        std::fs::write(&path, held_service.to_string()).unwrap();
        let refused = Config::load(&path).unwrap_err();
        assert!(
            refused.contains("hold.streamed is for start.gg"),
            "{refused}"
        );
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
