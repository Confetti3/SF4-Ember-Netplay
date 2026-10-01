//! A reference subscriber for Ember bridge events.
//!
//! It verifies each Standard Webhooks delivery before reading the body,
//! accepts only events from the configured bridge, records each event ID once
//! in a durable inbox, and answers 2xx only after that write (spec 20.4).
//! A worker then posts each event to a Discord channel webhook and Twitch
//! chat, retrying on its own schedule. Any other consumer of the bridge's
//! webhooks can work the same way.
use std::{
    collections::BTreeMap,
    net::SocketAddr,
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicI64, Ordering},
    },
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use axum::{
    Router,
    body::Bytes,
    extract::State,
    http::{HeaderMap, StatusCode},
    routing::post,
};
use ember_protocol::{
    EmberId,
    encoding::OriginPolicy,
    event::{Event, Kind},
    json,
    webhook::{self, Headers, Secret},
};
use rusqlite::{Connection, OptionalExtension, params};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use tokio::{net::TcpListener, sync::Notify, task::JoinHandle};
use zeroize::Zeroizing;

const MAX_BODY: usize = 64 * 1024;
const RETRY: &[u64] = &[2, 10, 30, 120, 600];
const GIVE_UP_SECS: u64 = 24 * 60 * 60;
const TWITCH_MAX: usize = 500;

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
    /// Display names for Ember IDs; others show their short fingerprint.
    #[serde(default)]
    pub names: BTreeMap<String, String>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Discord {
    /// The channel webhook URL, or `env:NAME`.
    pub webhook_url: String,
    #[serde(default = "default_username")]
    pub username: String,
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
}

fn default_twitch_api() -> String {
    "https://api.twitch.tv".into()
}

/// Resolves `env:NAME` to the variable's value.
fn resolve(value: &str) -> Result<Zeroizing<String>, String> {
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

#[derive(Clone, Default)]
pub struct Clock(Arc<AtomicI64>);

impl Clock {
    pub fn now(&self) -> u64 {
        let wall = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_secs() as i64);
        (wall + self.0.load(Ordering::Relaxed)).max(0) as u64
    }

    pub fn advance(&self, seconds: i64) {
        self.0.fetch_add(seconds, Ordering::Relaxed);
    }
}

struct Inner {
    config: Config,
    secrets: Vec<Secret>,
    discord_url: Option<Zeroizing<String>>,
    twitch_token: Option<Zeroizing<String>>,
    db: Mutex<Connection>,
    clock: Clock,
    wake: Notify,
    http: reqwest::Client,
}

#[derive(Clone)]
pub struct Notifier(Arc<Inner>);

impl Notifier {
    pub fn new(config: Config, clock: Clock) -> Result<Self, String> {
        let _ = rustls::crypto::ring::default_provider().install_default();
        let secrets = config
            .secrets
            .iter()
            .map(|secret| {
                Secret::parse(&resolve(secret)?)
                    .map_err(|_| "invalid subscription secret".to_owned())
            })
            .collect::<Result<Vec<_>, _>>()?;
        if secrets.is_empty() {
            return Err("at least one subscription secret is required".into());
        }
        let discord_url = config
            .discord
            .as_ref()
            .map(|d| resolve(&d.webhook_url))
            .transpose()?;
        let twitch_token = config
            .twitch
            .as_ref()
            .map(|t| resolve(&t.token))
            .transpose()?;
        let db = Connection::open(&config.database)
            .map_err(|error| format!("cannot open database: {error}"))?;
        db.execute_batch(
            "PRAGMA journal_mode = WAL; PRAGMA synchronous = FULL;
             CREATE TABLE IF NOT EXISTS inbox (
                 id TEXT PRIMARY KEY, type TEXT NOT NULL, body BLOB NOT NULL, received_at INTEGER NOT NULL,
                 discord_done INTEGER NOT NULL, twitch_done INTEGER NOT NULL,
                 attempts INTEGER NOT NULL DEFAULT 0, next_at INTEGER NOT NULL, last_error TEXT);
             CREATE TABLE IF NOT EXISTS matches (id TEXT PRIMARY KEY, label TEXT NOT NULL);",
        )
        .map_err(|error| format!("cannot prepare database: {error}"))?;
        let http = reqwest::Client::builder()
            .redirect(reqwest::redirect::Policy::none())
            .connect_timeout(Duration::from_secs(5))
            .timeout(Duration::from_secs(15))
            .user_agent(concat!("ember-notifier/", env!("CARGO_PKG_VERSION")))
            .build()
            .map_err(|error| error.to_string())?;
        Ok(Self(Arc::new(Inner {
            discord_url,
            twitch_token,
            secrets,
            config,
            db: Mutex::new(db),
            clock,
            wake: Notify::new(),
            http,
        })))
    }

    /// Serves `POST /webhook` on `listener` and starts the sender.
    pub fn start(
        &self,
        listener: TcpListener,
    ) -> std::io::Result<(SocketAddr, Vec<JoinHandle<()>>)> {
        let address = listener.local_addr()?;
        let app = Router::new()
            .route("/webhook", post(receive))
            .with_state(self.clone());
        let server = tokio::spawn(async move {
            let _ = axum::serve(listener, app).await;
        });
        let worker = tokio::spawn(self.clone().run());
        Ok((address, vec![server, worker]))
    }

    /// Verifies, filters and durably records one delivery.
    pub fn accept(&self, headers: &Headers, body: &[u8]) -> Result<bool, StatusCode> {
        let secrets: Vec<&Secret> = self.0.secrets.iter().collect();
        webhook::verify(&secrets, headers, body, self.0.clock.now())
            .map_err(|_| StatusCode::UNAUTHORIZED)?;
        // Only now is the body parsed, under the strict profile.
        let event: Event = json::parse_as(body, MAX_BODY).map_err(|_| StatusCode::BAD_REQUEST)?;
        let policy = if self.0.config.bridge_origin.starts_with("http://") {
            OriginPolicy::AllowLoopbackHttp
        } else {
            OriginPolicy::HttpsOnly
        };
        event.check(policy).map_err(|_| StatusCode::BAD_REQUEST)?;
        if event.source != self.0.config.bridge_origin || event.id != headers.id {
            return Err(StatusCode::BAD_REQUEST);
        }
        let db = self
            .0
            .db
            .lock()
            .map_err(|_| StatusCode::SERVICE_UNAVAILABLE)?;
        let now = self.0.clock.now();
        let fresh = db
            .execute(
                "INSERT OR IGNORE INTO inbox (id, type, body, received_at, discord_done, twitch_done, next_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?4)",
                params![
                    event.id,
                    event.kind,
                    body,
                    now,
                    self.0.config.discord.is_none(),
                    self.0.config.twitch.is_none()
                ],
            )
            .map_err(|_| StatusCode::SERVICE_UNAVAILABLE)?
            == 1;
        // Later events name their match or lobby by the label it was created with.
        let labelled = match event.kind() {
            Some(Kind::MatchCreated) => Some(("match_id", match_label(&event.data))),
            Some(Kind::LobbyCreated) => Some(("lobby_id", lobby_title(&event.data).to_owned())),
            _ => None,
        };
        if let (true, Some((key, label))) = (fresh, labelled) {
            let id = event
                .data
                .get(key)
                .and_then(Value::as_str)
                .unwrap_or_default();
            let _ = db.execute(
                "INSERT OR IGNORE INTO matches (id, label) VALUES (?1, ?2)",
                params![id, label.chars().take(100).collect::<String>()],
            );
        }
        drop(db);
        self.0.wake.notify_one();
        Ok(fresh)
    }

    async fn run(self) {
        loop {
            let now = self.0.clock.now();
            let due: Vec<(String, Vec<u8>, bool, bool, u32, u64)> = match self.0.db.lock() {
                Ok(db) => db
                    .prepare(
                        "SELECT id, body, discord_done, twitch_done, attempts, received_at FROM inbox
                         WHERE (discord_done = 0 OR twitch_done = 0) AND next_at <= ?1 ORDER BY received_at LIMIT 8",
                    )
                    .and_then(|mut statement| {
                        statement
                            .query_map([now], |row| {
                                Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?, row.get(4)?, row.get(5)?))
                            })?
                            .collect()
                    })
                    .unwrap_or_default(),
                Err(_) => Vec::new(),
            };
            if due.is_empty() {
                let _ = tokio::time::timeout(Duration::from_secs(1), self.0.wake.notified()).await;
                continue;
            }
            for (id, body, discord_done, twitch_done, attempts, received) in due {
                self.deliver(&id, &body, discord_done, twitch_done, attempts, received)
                    .await;
            }
        }
    }

    async fn deliver(
        &self,
        id: &str,
        body: &[u8],
        discord_done: bool,
        twitch_done: bool,
        attempts: u32,
        received: u64,
    ) {
        let Ok(event) = json::parse_as::<Event>(body, MAX_BODY) else {
            self.mark(id, true, true, attempts, None);
            return;
        };
        let Some(message) = self.render(&event) else {
            // Not an event this notifier announces.
            self.mark(id, true, true, attempts, None);
            return;
        };
        let mut retry = None;
        let discord = discord_done || {
            match self.discord(&message, &event).await {
                Sent::Done => true,
                Sent::Retry(error, after) => {
                    retry = Some((error, after));
                    false
                }
                Sent::Failed(error) => {
                    eprintln!("ember-notifier: Discord refused {id}: {error}");
                    true
                }
            }
        };
        let twitch = twitch_done || {
            match self.twitch(&message).await {
                Sent::Done => true,
                Sent::Retry(error, after) => {
                    retry = Some((error, after.max(retry.as_ref().map_or(0, |r| r.1))));
                    false
                }
                Sent::Failed(error) => {
                    eprintln!("ember-notifier: Twitch refused {id}: {error}");
                    true
                }
            }
        };
        let now = self.0.clock.now();
        let next = retry.map(|(error, after)| {
            let base = RETRY.get(attempts as usize).copied().unwrap_or(3600);
            (error, now + base.max(after))
        });
        match next {
            Some((error, at)) if at <= received + GIVE_UP_SECS => {
                self.mark_retry(id, discord, twitch, attempts + 1, at, &error)
            }
            Some((error, _)) => {
                eprintln!("ember-notifier: giving up on {id}: {error}");
                self.mark(id, true, true, attempts + 1, Some(&error));
            }
            None => self.mark(id, discord, twitch, attempts + 1, None),
        }
    }

    fn mark(&self, id: &str, discord: bool, twitch: bool, attempts: u32, error: Option<&str>) {
        if let Ok(db) = self.0.db.lock() {
            let _ = db.execute(
                "UPDATE inbox SET discord_done = ?1, twitch_done = ?2, attempts = ?3, last_error = ?4 WHERE id = ?5",
                params![discord, twitch, attempts, error, id],
            );
        }
    }

    fn mark_retry(
        &self,
        id: &str,
        discord: bool,
        twitch: bool,
        attempts: u32,
        at: u64,
        error: &str,
    ) {
        if let Ok(db) = self.0.db.lock() {
            let _ = db.execute(
                "UPDATE inbox SET discord_done = ?1, twitch_done = ?2, attempts = ?3, next_at = ?4, last_error = ?5 WHERE id = ?6",
                params![discord, twitch, attempts, at, error, id],
            );
        }
    }

    fn name(&self, id: &str) -> String {
        self.0.config.names.get(id).cloned().unwrap_or_else(|| {
            EmberId::parse(id).map_or_else(|_| "Unknown player".into(), |id| id.fingerprint())
        })
    }

    fn label(&self, match_id: &str) -> String {
        self.0
            .db
            .lock()
            .ok()
            .and_then(|db| {
                db.query_row(
                    "SELECT label FROM matches WHERE id = ?1",
                    [match_id],
                    |row| row.get(0),
                )
                .optional()
                .ok()
                .flatten()
            })
            .unwrap_or_else(|| "Match".into())
    }

    fn scores(&self, data: &serde_json::Map<String, Value>) -> String {
        data.get("scores")
            .and_then(Value::as_array)
            .map(|scores| {
                scores
                    .iter()
                    .map(|score| {
                        format!(
                            "{} {}",
                            self.name(
                                score
                                    .get("ember_id")
                                    .and_then(Value::as_str)
                                    .unwrap_or_default()
                            ),
                            score.get("wins").and_then(Value::as_u64).unwrap_or(0)
                        )
                    })
                    .collect::<Vec<_>>()
                    .join(" – ")
            })
            .unwrap_or_default()
    }

    /// The announcement for an event, or `None` for events not announced.
    pub fn render(&self, event: &Event) -> Option<Message> {
        let data = &event.data;
        let match_id = data
            .get("match_id")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let (title, text, color) = match event.kind()? {
            Kind::MatchCreated => {
                let names: Vec<String> = data
                    .get("participants")
                    .and_then(Value::as_array)?
                    .iter()
                    .map(|p| {
                        self.name(
                            p.get("ember_id")
                                .and_then(Value::as_str)
                                .unwrap_or_default(),
                        )
                    })
                    .collect();
                let label = match_label(data);
                let first_to = data
                    .get("games_to_win")
                    .and_then(Value::as_u64)
                    .unwrap_or(1);
                (
                    label,
                    format!(
                        "{} vs {}, first to {first_to}",
                        names.first()?,
                        names.get(1)?
                    ),
                    0x5865F2,
                )
            }
            Kind::ScoreChanged => (self.label(match_id), self.scores(data), 0x3BA55C),
            Kind::GameConfirmed if data.get("outcome").and_then(Value::as_str) == Some("draw") => (
                self.label(match_id),
                "Draw. The game is replayed.".into(),
                0x99AAB5,
            ),
            Kind::MatchCompleted => {
                let winner = self.name(data.get("winner_id").and_then(Value::as_str)?);
                (
                    self.label(match_id),
                    format!("{winner} wins the set. {}", self.scores(data)),
                    0xF1C40F,
                )
            }
            Kind::MatchCancelled => (self.label(match_id), "Match cancelled.".into(), 0xED4245),
            Kind::NeedsReview => (
                self.label(match_id),
                "Waiting for an organizer to review the result.".into(),
                0xFAA61A,
            ),
            Kind::MatchCorrected => (
                self.label(match_id),
                "The organizer corrected the result.".into(),
                0xFAA61A,
            ),
            Kind::LobbyCreated => {
                let first_to = data
                    .get("games_to_win")
                    .and_then(Value::as_u64)
                    .unwrap_or(1);
                let rotation = match data.get("rotation").and_then(Value::as_str)? {
                    "winner_stays" => "winner stays",
                    "loser_stays" => "loser stays",
                    _ => "both players rotate",
                };
                (
                    lobby_title(data).to_owned(),
                    format!(
                        "Lobby open: first to {first_to}, {rotation}. Ask to join the queue to play."
                    ),
                    0x5865F2,
                )
            }
            // The set's own match events announce who plays and the score;
            // this adds what the rotation did.
            Kind::LobbySetCompleted => {
                let lobby_id = data.get("lobby_id").and_then(Value::as_str)?;
                let winner = self.name(data.get("winner_id").and_then(Value::as_str)?);
                let streak = data
                    .get("streak")
                    .and_then(|streak| streak.get("sets"))
                    .and_then(Value::as_u64)
                    .filter(|sets| *sets >= 2)
                    .map(|sets| format!(", {sets} sets in a row"))
                    .unwrap_or_default();
                let next: Vec<String> = data
                    .get("queue")
                    .and_then(Value::as_array)
                    .map(|queue| {
                        queue
                            .iter()
                            .take(3)
                            .filter_map(Value::as_str)
                            .map(|id| self.name(id))
                            .collect()
                    })
                    .unwrap_or_default();
                let queue = if next.is_empty() {
                    "Nobody is waiting in the queue.".to_owned()
                } else {
                    format!("Next in the queue: {}.", next.join(", "))
                };
                (
                    self.label(lobby_id),
                    format!("{winner} wins the set{streak}. {queue}"),
                    0xF1C40F,
                )
            }
            Kind::LobbyClosed => (
                self.label(data.get("lobby_id").and_then(Value::as_str)?),
                "Lobby closed.".into(),
                0x99AAB5,
            ),
            _ => return None,
        };
        Some(Message { title, text, color })
    }

    async fn discord(&self, message: &Message, event: &Event) -> Sent {
        let (Some(config), Some(url)) = (&self.0.config.discord, &self.0.discord_url) else {
            return Sent::Done;
        };
        let body = json!({
            "username": config.username,
            // Event text never pings anyone.
            "allowed_mentions": { "parse": [] },
            "embeds": [{
                "title": escape_markdown(&message.title),
                "description": escape_markdown(&message.text),
                "color": message.color,
                "timestamp": event.time,
            }],
        });
        let separator = if url.contains('?') { '&' } else { '?' };
        let request = self
            .0
            .http
            .post(format!("{}{separator}wait=true", url.as_str()))
            .header("content-type", "application/json")
            .body(serde_json::to_vec(&body).unwrap_or_default());
        classify(
            request.send().await,
            Reset::After("x-ratelimit-reset-after"),
            self.0.clock.now(),
        )
        .await
    }

    async fn twitch(&self, message: &Message) -> Sent {
        let (Some(config), Some(token)) = (&self.0.config.twitch, &self.0.twitch_token) else {
            return Sent::Done;
        };
        let mut text = format!("{}: {}", message.title, message.text);
        if text.chars().count() > TWITCH_MAX {
            text = text.chars().take(TWITCH_MAX - 1).collect::<String>() + "…";
        }
        let body = json!({
            "broadcaster_id": config.broadcaster_id,
            "sender_id": config.sender_id,
            "message": text,
        });
        let request = self
            .0
            .http
            .post(format!(
                "{}/helix/chat/messages",
                config.api_base.trim_end_matches('/')
            ))
            .bearer_auth(token.as_str())
            .header("client-id", &config.client_id)
            .header("content-type", "application/json")
            .body(serde_json::to_vec(&body).unwrap_or_default());
        match request.send().await {
            // Twitch answers 200 for a message it then drops (AutoMod, a ban,
            // slow mode), so success is what the answer says.
            Ok(response) if response.status().is_success() => {
                match bounded_body(response, MAX_TWITCH_ANSWER).await {
                    Some(body) => twitch_outcome(&body),
                    None => Sent::Failed("unreadable response".into()),
                }
            }
            other => classify(other, Reset::At("ratelimit-reset"), self.0.clock.now()).await,
        }
    }
}

const MAX_TWITCH_ANSWER: usize = 64 * 1024;

async fn bounded_body(mut response: reqwest::Response, limit: usize) -> Option<Vec<u8>> {
    let mut body = Vec::new();
    while let Some(chunk) = response.chunk().await.ok()? {
        if body.len() + chunk.len() > limit {
            return None;
        }
        body.extend_from_slice(&chunk);
    }
    Some(body)
}

/// Reads `POST /helix/chat/messages`: `{"data":[{"is_sent":..,"drop_reason":..}]}`.
fn twitch_outcome(body: &[u8]) -> Sent {
    let Ok(value) = serde_json::from_slice::<Value>(body) else {
        return Sent::Failed("unexpected response".into());
    };
    let entry = &value["data"][0];
    match entry["is_sent"].as_bool() {
        Some(true) => Sent::Done,
        Some(false) => {
            // The code is Twitch's text: keep it short and plain for the log.
            let code: String = entry["drop_reason"]["code"]
                .as_str()
                .unwrap_or("unknown")
                .chars()
                .filter(|c| c.is_ascii_alphanumeric() || *c == '_')
                .take(64)
                .collect();
            Sent::Failed(format!("dropped: {code}"))
        }
        None => Sent::Failed("unexpected response".into()),
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Message {
    pub title: String,
    pub text: String,
    pub color: u32,
}

enum Sent {
    Done,
    Retry(String, u64),
    Failed(String),
}

/// How a service says when its rate limit resets, besides `Retry-After`.
#[derive(Clone, Copy)]
enum Reset {
    /// Seconds from now (Discord's `X-RateLimit-Reset-After`).
    After(&'static str),
    /// A Unix time in seconds (Twitch's `Ratelimit-Reset`).
    At(&'static str),
}

/// The wait in seconds a rate-limited answer asks for, at most an hour.
fn retry_delay(headers: &reqwest::header::HeaderMap, reset: Reset, now: u64) -> u64 {
    let number = |name: &str| {
        headers
            .get(name)
            .and_then(|value| value.to_str().ok())
            .and_then(|value| value.trim().parse::<f64>().ok())
            .filter(|value| value.is_finite())
    };
    let seconds = number("retry-after").or_else(|| match reset {
        Reset::After(name) => number(name),
        Reset::At(name) => number(name).map(|at| at - now as f64),
    });
    seconds.map_or(0, |seconds| seconds.ceil().clamp(0.0, 3600.0) as u64)
}

async fn classify(response: reqwest::Result<reqwest::Response>, reset: Reset, now: u64) -> Sent {
    let response = match response {
        Ok(response) => response,
        Err(_) => return Sent::Retry("unreachable".into(), 0),
    };
    let status = response.status();
    let after = retry_delay(response.headers(), reset, now);
    if status.is_success() {
        Sent::Done
    } else if status.as_u16() == 429 || status.is_server_error() {
        Sent::Retry(format!("status {}", status.as_u16()), after)
    } else {
        Sent::Failed(format!("status {}", status.as_u16()))
    }
}

/// A match's display label: its round label, or its lobby and set number.
fn match_label(data: &serde_json::Map<String, Value>) -> String {
    let metadata = data.get("metadata");
    let field = |key: &str| metadata.and_then(|m| m.get(key)).and_then(Value::as_str);
    match (field("round_label"), field("lobby_set")) {
        (Some(label), _) => label.to_owned(),
        (None, Some(set)) => format!("{}, set {set}", field("title").unwrap_or("Lobby")),
        (None, None) => "Match".to_owned(),
    }
}

fn lobby_title(data: &serde_json::Map<String, Value>) -> &str {
    data.get("metadata")
        .and_then(|metadata| metadata.get("title"))
        .and_then(Value::as_str)
        .unwrap_or("Lobby")
}

/// Keeps names and labels from being read as Discord formatting.
fn escape_markdown(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for ch in text.chars() {
        if matches!(
            ch,
            '\\' | '*' | '_' | '~' | '`' | '|' | '>' | '[' | ']' | '(' | ')' | '#' | '@'
        ) {
            out.push('\\');
        }
        out.push(ch);
    }
    out
}

async fn receive(State(notifier): State<Notifier>, headers: HeaderMap, body: Bytes) -> StatusCode {
    if body.len() > MAX_BODY {
        return StatusCode::PAYLOAD_TOO_LARGE;
    }
    let get = |name: &str| {
        headers
            .get(name)
            .and_then(|value| value.to_str().ok())
            .unwrap_or_default()
            .to_owned()
    };
    let headers = Headers {
        id: get(webhook::HEADER_ID),
        timestamp: get(webhook::HEADER_TIMESTAMP),
        signature: get(webhook::HEADER_SIGNATURE),
    };
    match notifier.accept(&headers, &body) {
        // A duplicate is acknowledged too: it is already recorded.
        Ok(_) => StatusCode::NO_CONTENT,
        Err(status) => status,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn markdown_is_escaped() {
        assert_eq!(
            escape_markdown("**Kate** @everyone"),
            "\\*\\*Kate\\*\\* \\@everyone"
        );
    }

    #[test]
    fn rate_limit_resets_are_read_by_their_meaning() {
        use reqwest::header::{HeaderMap, HeaderValue};
        let now = 1_800_000_000;
        let mut twitch = HeaderMap::new();
        twitch.insert("ratelimit-reset", HeaderValue::from_static("1800000010"));
        assert_eq!(retry_delay(&twitch, Reset::At("ratelimit-reset"), now), 10);
        let mut discord = HeaderMap::new();
        discord.insert("x-ratelimit-reset-after", HeaderValue::from_static("1.2"));
        assert_eq!(
            retry_delay(&discord, Reset::After("x-ratelimit-reset-after"), now),
            2
        );
        // Retry-After wins, and a reset in the past means no wait.
        twitch.insert("retry-after", HeaderValue::from_static("3"));
        assert_eq!(retry_delay(&twitch, Reset::At("ratelimit-reset"), now), 3);
        let mut past = HeaderMap::new();
        past.insert("ratelimit-reset", HeaderValue::from_static("1700000000"));
        assert_eq!(retry_delay(&past, Reset::At("ratelimit-reset"), now), 0);
    }

    #[test]
    fn a_dropped_twitch_message_is_not_delivered() {
        let sent = br#"{"data":[{"message_id":"m1","is_sent":true,"drop_reason":null}]}"#;
        assert!(matches!(twitch_outcome(sent), Sent::Done));
        let dropped = br#"{"data":[{"message_id":"","is_sent":false,"drop_reason":{"code":"msg_rejected","message":"AutoMod"}}]}"#;
        assert!(
            matches!(twitch_outcome(dropped), Sent::Failed(reason) if reason == "dropped: msg_rejected")
        );
        assert!(matches!(twitch_outcome(b""), Sent::Failed(_)));
    }
}
