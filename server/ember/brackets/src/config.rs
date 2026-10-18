//! The connector's configuration file. Secrets are never in it, only the
//! paths of the files that hold them.
use std::path::{Path, PathBuf};

use ember_protocol::{
    encoding::{OriginPolicy, check_origin},
    event::{is_rfc3339_utc, rfc3339},
    json,
    matches::games_to_win_valid,
};
use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

use crate::schedule::Window;

const MAX_CONFIG: usize = 64 * 1024;
/// Challonge's free tier: 500 requests a month per developer app.
const DEFAULT_BUDGET: u32 = 500;
const DEFAULT_POLL_SECS: u64 = 120;
const DEFAULT_MAX_POLL_SECS: u64 = 600;
const MIN_POLL_SECS: u64 = 30;
/// Tournament and set IDs go into URLs and `external_match_id`.
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
    /// Requests each site credential may make in a calendar month (UTC).
    /// At 80% the connector warns; at 100% it stops reading brackets but
    /// still reports results.
    #[serde(default = "default_budget")]
    pub monthly_request_budget: u32,
    pub tournaments: Vec<Tournament>,
    /// Lets a tournament's `api_base` be a loopback `http` origin. Local
    /// tests only: in a deployment the organizer's credential goes to
    /// Challonge over https and nowhere else.
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
    pub site: SiteKind,
    /// The tournament's ID or URL slug on the site.
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
    /// The site's API origin, when not its public one: for Challonge an
    /// https origin on challonge.com, or loopback http for local tests.
    #[serde(default)]
    pub api_base: Option<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SiteKind {
    Challonge,
}

impl SiteKind {
    pub fn name(self) -> &'static str {
        match self {
            Self::Challonge => "challonge",
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

/// Set lengths by round. The site does not say how long a set is.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Rounds {
    /// First to this many games, unless a rule says otherwise.
    pub games_to_win: u8,
    /// The first rule that covers a round sets its length.
    #[serde(default)]
    pub rules: Vec<RoundRule>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RoundRule {
    pub from_round: u32,
    /// The last round covered; every later round when absent.
    #[serde(default)]
    pub to_round: Option<u32>,
    #[serde(default)]
    pub side: Side,
    pub games_to_win: u8,
}

/// Which rounds a rule counts. Challonge numbers the losers bracket's rounds
/// below zero and every other round above it, so `winners` also covers
/// single elimination, round robin and swiss.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Side {
    Winners,
    Losers,
    #[default]
    Any,
}

impl Rounds {
    /// The set length of `round`, as the site numbers it.
    pub fn games_to_win(&self, round: i32) -> u8 {
        let side = if round < 0 {
            Side::Losers
        } else {
            Side::Winners
        };
        let number = round.unsigned_abs();
        self.rules
            .iter()
            .find(|rule| {
                (rule.side == Side::Any || rule.side == side)
                    && number >= rule.from_round
                    && rule.to_round.is_none_or(|to| number <= to)
            })
            .map_or(self.games_to_win, |rule| rule.games_to_win)
    }

    fn check(&self) -> Result<(), String> {
        let lengths_valid = std::iter::once(self.games_to_win)
            .chain(self.rules.iter().map(|rule| rule.games_to_win))
            .all(games_to_win_valid);
        if !lengths_valid {
            return Err("games_to_win is 1 to 10".into());
        }
        let ranges_valid = self.rules.iter().all(|rule| {
            rule.from_round >= 1 && rule.to_round.is_none_or(|to| to >= rule.from_round)
        });
        if !ranges_valid {
            return Err("a round rule starts at round 1 or later and ends no earlier".into());
        }
        Ok(())
    }
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
        format!("{}:{}", self.site.name(), self.tournament)
    }

    pub(crate) fn window(&self) -> Result<Window, String> {
        let read = |field: &Option<String>, name: &str| {
            field
                .as_deref()
                .map(|text| {
                    parse_utc(text).ok_or_else(|| {
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

/// Whether the organizer's Challonge credential may go to `origin`: an
/// https origin on challonge.com or one of its subdomains, with no port,
/// path or user info, or a loopback http origin when tests allow it. The
/// bridge holds its own Challonge settings to the same rule.
pub(crate) fn is_challonge_origin(origin: &str, allow_loopback_http: bool) -> bool {
    let policy = if allow_loopback_http {
        OriginPolicy::AllowLoopbackHttp
    } else {
        OriginPolicy::HttpsOnly
    };
    let on_challonge = origin
        .strip_prefix("https://")
        .is_some_and(|host| host == "challonge.com" || host.ends_with(".challonge.com"));
    check_origin(origin, policy).is_ok() && (on_challonge || origin.starts_with("http://"))
}

/// Unix seconds of `YYYY-MM-DDTHH:MM:SSZ`, the one form a window takes.
/// The protocol's formatter checks the date back, so 2026-02-29 is refused.
fn parse_utc(text: &str) -> Option<u64> {
    if !is_rfc3339_utc(text) {
        return None;
    }
    let field = |range: std::ops::Range<usize>| text[range].parse::<i64>().ok();
    let (year, month, day) = (field(0..4)?, field(5..7)?, field(8..10)?);
    let (hour, minute, second) = (field(11..13)?, field(14..16)?, field(17..19)?);
    // Howard Hinnant's days-from-civil.
    let shifted_year = if month <= 2 { year - 1 } else { year };
    let era = shifted_year.div_euclid(400);
    let year_of_era = shifted_year.rem_euclid(400);
    let day_of_year = (153 * ((month + 9) % 12) + 2) / 5 + day - 1;
    let day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    let days = era * 146_097 + day_of_era - 719_468;
    let unix = u64::try_from(days * 86_400 + hour * 3_600 + minute * 60 + second).ok()?;
    (rfc3339(unix) == text).then_some(unix)
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
            if !is_slug(&tournament.tournament) {
                return Err(format!(
                    "tournament {:?} is not an ID or URL slug",
                    tournament.tournament
                ));
            }
            if !keys.insert(tournament.key()) {
                return Err(format!("{} is listed twice", tournament.key()));
            }
            tournament
                .rounds
                .check()
                .map_err(|error| format!("{}: {error}", tournament.key()))?;
            tournament.window()?;
            if let Some(origin) = &tournament.api_base
                && !is_challonge_origin(origin, self.allow_loopback_http)
            {
                return Err(format!(
                    "{}: api_base is an https origin on challonge.com",
                    tournament.key()
                ));
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rule(from: u32, to: Option<u32>, side: Side, games: u8) -> RoundRule {
        RoundRule {
            from_round: from,
            to_round: to,
            side,
            games_to_win: games,
        }
    }

    #[test]
    fn the_first_rule_that_covers_a_round_sets_its_length() {
        let rounds = Rounds {
            games_to_win: 2,
            rules: vec![
                rule(5, None, Side::Winners, 4),
                rule(3, None, Side::Any, 3),
                rule(1, Some(2), Side::Losers, 1),
            ],
        };
        let lengths: Vec<u8> = [1, 2, 3, 4, 5, 9, -1, -2, -3, -7]
            .into_iter()
            .map(|round| rounds.games_to_win(round))
            .collect();
        assert_eq!(lengths, [2, 2, 3, 3, 4, 4, 1, 1, 3, 3]);
        // Round 0 is not a losers round.
        assert_eq!(rounds.games_to_win(0), 2);
    }

    #[test]
    fn the_organizers_credential_goes_to_challonge_only() {
        for origin in [
            "https://api.challonge.com",
            "https://challonge.com",
            "https://staging.api.challonge.com",
        ] {
            assert!(is_challonge_origin(origin, false), "{origin}");
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
            assert!(!is_challonge_origin(origin, false), "{origin}");
        }
        // Loopback http is for tests, and only loopback.
        assert!(is_challonge_origin("http://127.0.0.1:9000", true));
        assert!(is_challonge_origin("http://localhost:9000", true));
        assert!(!is_challonge_origin("http://api.challonge.com", true));
        assert!(!is_challonge_origin("http://10.0.0.5:9000", true));
    }

    #[test]
    fn window_times_are_utc_in_one_form() {
        for (text, unix) in [
            ("1970-01-01T00:00:00Z", 0),
            ("2026-10-18T19:00:00Z", 1_792_350_000),
            ("2024-02-29T23:59:59Z", 1_709_251_199),
            ("2030-01-01T03:00:00Z", 1_893_466_800),
        ] {
            assert_eq!(parse_utc(text), Some(unix), "{text}");
        }
        for text in [
            "2026-10-18T19:00:00",
            "2026-10-18T19:00:00.750Z",
            "2026-10-18T19:00:00+00:00",
            "2026-10-18 19:00:00Z",
            "2026-02-29T00:00:00Z",
            "2026-13-01T00:00:00Z",
            "2026-10-18T24:00:00Z",
            "2026-10-18T19:60:00Z",
            "1969-12-31T23:59:59Z",
        ] {
            assert_eq!(parse_utc(text), None, "{text}");
        }
    }

    #[test]
    fn round_rules_are_checked() {
        let with = |rules| Rounds {
            games_to_win: 2,
            rules,
        };
        assert!(with(vec![rule(1, None, Side::Any, 10)]).check().is_ok());
        assert!(with(vec![rule(0, None, Side::Any, 3)]).check().is_err());
        assert!(with(vec![rule(4, Some(3), Side::Any, 3)]).check().is_err());
        assert!(with(vec![rule(1, None, Side::Any, 11)]).check().is_err());
        assert!(
            Rounds {
                games_to_win: 0,
                rules: vec![]
            }
            .check()
            .is_err()
        );
    }

    #[test]
    fn a_file_reads_with_defaults_and_its_own_directory() {
        let dir =
            std::env::temp_dir().join(format!("ember-brackets-config-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
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
                "site": "challonge",
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
        assert_eq!(config.tournaments[0].rounds.rules[0].side, Side::Any);

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
