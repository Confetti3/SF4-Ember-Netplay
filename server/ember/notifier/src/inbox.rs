//! The durable inbox: verified deliveries are recorded once, then a worker
//! posts each to the configured services, retrying on its own schedule.
use std::time::Duration;

use axum::{
    body::Bytes,
    extract::State,
    http::{HeaderMap, StatusCode},
};
use ember_protocol::{
    encoding::OriginPolicy,
    event::{Event, Kind},
    json,
    webhook::{self, Headers, Secret},
};
use rusqlite::params;
use serde_json::Value;

use crate::{
    Notifier,
    config::MAX_BODY,
    format::{match_label, title_of},
    sent::Sent,
};

const RETRY: &[u64] = &[2, 10, 30, 120, 600];
const GIVE_UP_SECS: u64 = 24 * 60 * 60;

impl Notifier {
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
        // Later events name their match, lobby or tournament by the label it
        // was created with.
        let labelled = match event.kind() {
            Some(Kind::MatchCreated) => Some(("match_id", match_label(&event.data))),
            Some(Kind::LobbyCreated) => {
                Some(("lobby_id", title_of(&event.data, "Lobby").to_owned()))
            }
            Some(Kind::TournamentCreated) => Some((
                "tournament_id",
                title_of(&event.data, "Tournament").to_owned(),
            )),
            _ => None,
        };
        let mut db = self
            .0
            .db
            .lock()
            .map_err(|_| StatusCode::SERVICE_UNAVAILABLE)?;
        let now = self.0.clock.now();
        // The event and the label it brings commit together, so a retried
        // delivery never finds one without the other.
        let unavailable = |_| StatusCode::SERVICE_UNAVAILABLE;
        let tx = db.transaction().map_err(unavailable)?;
        let fresh = tx
            .execute(
                "INSERT OR IGNORE INTO inbox (id, type, body, received_at, discord_done, twitch_done, next_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?4)",
                params![
                    event.id,
                    event.kind,
                    body,
                    now,
                    self.0.discord_url.is_none(),
                    self.0.config.twitch.is_none()
                ],
            )
            .map_err(unavailable)?
            == 1;
        if let (true, Some((key, label))) = (fresh, labelled) {
            let id = event
                .data
                .get(key)
                .and_then(Value::as_str)
                .unwrap_or_default();
            tx.execute(
                "INSERT OR IGNORE INTO matches (id, label) VALUES (?1, ?2)",
                params![id, label.chars().take(100).collect::<String>()],
            )
            .map_err(unavailable)?;
        }
        tx.commit().map_err(unavailable)?;
        drop(db);
        self.0.wake.notify_one();
        Ok(fresh)
    }

    pub(crate) async fn run(self) {
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
}

pub(crate) async fn receive(
    State(notifier): State<Notifier>,
    headers: HeaderMap,
    body: Bytes,
) -> StatusCode {
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
