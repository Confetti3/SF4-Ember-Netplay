//! The room supervisor's loopback API (docs/design/PUBLIC_ROOMS.md,
//! "Supervisor API"): create a room, list the rooms it hosts, delete one.
//! Every call carries the shared secret as a bearer token.
use std::time::Duration;

use ember_protocol::{
    play::MAX_INVITATION,
    rooms::{MAX_ROOM_BANS, ROOM_LIMIT, UNSUPPORTED_BUILD},
};
use serde::{Deserialize, Serialize};

use crate::{AppState, util::outbound_client};

/// Starting a room host takes longer than an ordinary request.
const CREATE_TIMEOUT: Duration = Duration::from_secs(30);
/// What a supervisor's answer to a create request may weigh.
const MAX_ANSWER: usize = 1024 * 1024;
/// The most a room's `details` can weigh in `GET /rooms` (`MAX_DETAILS_BYTES`
/// in ember-rooms, a separate crate; keep them equal).
const MAX_DETAILS_REPORT: usize = 2048;
/// The most one room can weigh in `GET /rooms`: `MAX_ROOM_BANS` Ember IDs (57
/// bytes, 60 with quotes and a comma), its invitation, its details and about
/// 300 bytes for the other fields.
const MAX_ROOM_REPORT: usize = MAX_ROOM_BANS * 60 + MAX_INVITATION + MAX_DETAILS_REPORT + 300;
/// The most rooms a supervisor can be configured for (`MAX_ROOMS_LIMIT` in
/// ember-rooms).
const MAX_ROOMS_REPORTED: usize = 1024;
/// What the answer to `GET /rooms` may weigh: a full report of every room, so
/// a busy supervisor's report is never refused for its size (about 36 MB at
/// the extreme; with the default 8 rooms it is under 300 KB).
const MAX_LIST_ANSWER: usize = MAX_ROOMS_REPORTED * MAX_ROOM_REPORT;

pub struct Supervisor<'a> {
    base: &'a str,
    secret: &'a str,
}

/// What a call to the supervisor can come to.
#[derive(Debug, PartialEq, Eq)]
pub enum Failure {
    /// The supervisor refused the room for a reason a player can act on:
    /// `unsupported_build` or `room_limit`.
    Refused(&'static str),
    /// Unreachable, slow or answered something unusable.
    Unavailable,
}

#[derive(Serialize)]
pub struct Create<'a> {
    pub room_id: &'a str,
    pub name: &'a str,
    pub capacity: u8,
    pub build_id: &'a str,
    pub creator: &'a str,
    pub bridge_id: &'a str,
    pub ticket_key: &'a str,
    pub ticket_kid: &'a str,
}

#[derive(Deserialize)]
pub struct Created {
    pub invitation: String,
    pub region: String,
}

/// One room in `GET /rooms`.
#[derive(Deserialize)]
pub struct Reported {
    pub room_id: String,
    pub members: u32,
    pub tables_playing: u32,
    #[serde(default)]
    pub invitation: String,
    #[serde(default)]
    pub banned: Vec<String>,
    /// Whether the supervisor has ever seen a member in the room. A supervisor
    /// that does not say is judged by the current count.
    #[serde(default)]
    pub opened: Option<bool>,
    /// What the room host said about its room, as the supervisor passed it
    /// on. Any JSON value parses, so what is wrong with it is judged per
    /// field later (`details`) and never fails the report.
    #[serde(default)]
    pub details: Option<serde_json::Value>,
}

/// A refusal's body: `{"reason": "..."}`, or `{"error": "..."}`.
#[derive(Deserialize)]
struct Refusal {
    #[serde(default)]
    reason: Option<String>,
    #[serde(default)]
    error: Option<String>,
}

/// The body of `response`, read chunk by chunk: the read stops as soon as it
/// would pass `limit` bytes, so a large answer is never held in memory.
async fn read_bounded(mut response: reqwest::Response, limit: usize) -> Result<Vec<u8>, Failure> {
    if response
        .content_length()
        .is_some_and(|length| length > limit as u64)
    {
        return Err(Failure::Unavailable);
    }
    let mut bytes = Vec::new();
    while let Some(chunk) = response.chunk().await.map_err(|_| Failure::Unavailable)? {
        if chunk.len() > limit - bytes.len() {
            return Err(Failure::Unavailable);
        }
        bytes.extend_from_slice(&chunk);
    }
    Ok(bytes)
}

impl<'a> Supervisor<'a> {
    /// The supervisor `state` is configured for, or none when public rooms
    /// are off.
    pub fn of(state: &'a AppState) -> Option<Self> {
        Some(Self {
            base: state
                .config
                .rooms
                .as_ref()?
                .supervisor_url
                .trim_end_matches('/'),
            secret: state
                .integrations
                .rooms_supervisor_secret
                .as_deref()?
                .as_str(),
        })
    }

    fn client() -> Result<reqwest::Client, Failure> {
        outbound_client().build().map_err(|_| Failure::Unavailable)
    }

    pub async fn create(&self, room: &Create<'_>) -> Result<Created, Failure> {
        let body = serde_json::to_vec(room).map_err(|_| Failure::Unavailable)?;
        let response = Self::client()?
            .post(format!("{}/rooms", self.base))
            .bearer_auth(self.secret)
            .timeout(CREATE_TIMEOUT)
            .header("content-type", "application/json")
            .body(body)
            .send()
            .await
            .map_err(|_| Failure::Unavailable)?;
        let status = response.status();
        let bytes = read_bounded(response, MAX_ANSWER).await?;
        if status.is_success() {
            return serde_json::from_slice(&bytes).map_err(|_| Failure::Unavailable);
        }
        let refusal: Refusal = serde_json::from_slice(&bytes).map_err(|_| Failure::Unavailable)?;
        match refusal.reason.or(refusal.error).as_deref() {
            Some(UNSUPPORTED_BUILD) => Err(Failure::Refused(UNSUPPORTED_BUILD)),
            Some(ROOM_LIMIT) => Err(Failure::Refused(ROOM_LIMIT)),
            _ => Err(Failure::Unavailable),
        }
    }

    pub async fn list(&self) -> Result<Vec<Reported>, Failure> {
        let response = Self::client()?
            .get(format!("{}/rooms", self.base))
            .bearer_auth(self.secret)
            .send()
            .await
            .map_err(|_| Failure::Unavailable)?;
        if !response.status().is_success() {
            return Err(Failure::Unavailable);
        }
        let bytes = read_bounded(response, MAX_LIST_ANSWER).await?;
        serde_json::from_slice(&bytes).map_err(|_| Failure::Unavailable)
    }

    /// Best effort: a room that outlives this call closes on its own when
    /// nobody arrives.
    /// Closes a room; a room the supervisor no longer has counts as closed.
    pub async fn delete(&self, room_id: &str) -> Result<(), Failure> {
        let response = Self::client()?
            .delete(format!("{}/rooms/{room_id}", self.base))
            .bearer_auth(self.secret)
            .send()
            .await
            .map_err(|_| Failure::Unavailable)?;
        let status = response.status();
        if status.is_success() || status == reqwest::StatusCode::NOT_FOUND {
            Ok(())
        } else {
            Err(Failure::Unavailable)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn response(body: Vec<u8>) -> reqwest::Response {
        axum::http::Response::new(body).into()
    }

    #[tokio::test]
    async fn a_body_is_read_up_to_the_limit_and_refused_past_it() {
        let at_limit = read_bounded(response(vec![b'a'; 64]), 64).await;
        assert_eq!(at_limit.unwrap().len(), 64);
        let empty = read_bounded(response(Vec::new()), 0).await;
        assert_eq!(empty.unwrap().len(), 0);
        let over = read_bounded(response(vec![b'a'; 65]), 64).await;
        assert_eq!(over.unwrap_err(), Failure::Unavailable);
    }
}
