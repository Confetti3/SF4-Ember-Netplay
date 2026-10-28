//! What the services that call a bridge with a provider credential share:
//! the HTTP client, the bounded body read, the bridge client that sends a
//! request and reads the answer or the error shape, and the clock. Behind
//! the `client` feature, which is off by default so the helper does not
//! compile an HTTP client it does not use.
use std::{
    sync::{
        Arc,
        atomic::{AtomicI64, Ordering},
    },
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use serde::de::DeserializeOwned;
use zeroize::Zeroizing;

use crate::{
    api::{ApiError, ErrorBody},
    json,
};

/// An HTTP client that follows no redirects and gives up on a connection
/// after five seconds and on a request after `timeout`.
pub fn http_client(user_agent: &str, timeout: Duration) -> Result<reqwest::Client, String> {
    reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::none())
        .connect_timeout(Duration::from_secs(5))
        .timeout(timeout)
        .user_agent(user_agent)
        .build()
        .map_err(|error| error.to_string())
}

/// The body, or `None` when it fails or is longer than `limit`.
pub async fn bounded_body(mut response: reqwest::Response, limit: usize) -> Option<Vec<u8>> {
    let mut body = Vec::new();
    while let Some(chunk) = response.chunk().await.ok()? {
        if body.len() + chunk.len() > limit {
            return None;
        }
        body.extend_from_slice(&chunk);
    }
    Some(body)
}

/// Why a bridge call did not give the answer asked for.
#[derive(Debug)]
pub enum Failure {
    /// The bridge answered with its error shape.
    Refused(Box<ErrorBody>),
    /// It could not be reached, or its answer was not one the bridge sends.
    Unavailable,
}

/// A bridge, called with a provider credential.
pub struct BridgeClient {
    http: reqwest::Client,
    api: String,
    credential: Zeroizing<String>,
}

impl BridgeClient {
    pub fn new(http: reqwest::Client, api: &str, credential: Zeroizing<String>) -> Self {
        Self {
            http,
            api: api.trim_end_matches('/').to_owned(),
            credential,
        }
    }

    /// `path` on the bridge, for a request that carries no credential.
    pub fn open(&self, path: &str) -> reqwest::RequestBuilder {
        self.http.get(format!("{}{path}", self.api))
    }

    pub fn get(&self, path: &str) -> reqwest::RequestBuilder {
        self.open(path).bearer_auth(self.credential.as_str())
    }

    pub fn post(&self, path: &str) -> reqwest::RequestBuilder {
        self.http
            .post(format!("{}{path}", self.api))
            .bearer_auth(self.credential.as_str())
    }

    pub fn put(&self, path: &str) -> reqwest::RequestBuilder {
        self.http
            .put(format!("{}{path}", self.api))
            .bearer_auth(self.credential.as_str())
    }

    /// Sends the request and reads an answer of at most `limit` bytes: the
    /// typed answer on success, otherwise the bridge's error shape.
    pub async fn answer<T: DeserializeOwned>(
        &self,
        request: reqwest::RequestBuilder,
        limit: usize,
    ) -> Result<T, Failure> {
        let response = request.send().await.map_err(|_| Failure::Unavailable)?;
        let success = response.status().is_success();
        let body = bounded_body(response, limit)
            .await
            .ok_or(Failure::Unavailable)?;
        if success {
            return json::parse_as(&body, limit).map_err(|_| Failure::Unavailable);
        }
        let error: ApiError = json::parse_as(&body, limit).map_err(|_| Failure::Unavailable)?;
        Err(Failure::Refused(Box::new(error.error)))
    }
}

/// Unix seconds: the wall clock, or for tests a clock that stands still
/// until it is moved.
#[derive(Clone)]
pub struct Clock {
    offset: Arc<AtomicI64>,
    wall: bool,
}

impl Default for Clock {
    fn default() -> Self {
        Self {
            offset: Arc::default(),
            wall: true,
        }
    }
}

impl Clock {
    /// A clock that reads `at` until it is moved.
    pub fn frozen(at: u64) -> Self {
        Self {
            offset: Arc::new(AtomicI64::new(at as i64)),
            wall: false,
        }
    }

    pub fn now(&self) -> u64 {
        let wall = if self.wall {
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map_or(0, |elapsed| elapsed.as_secs() as i64)
        } else {
            0
        };
        (wall + self.offset.load(Ordering::Relaxed)).max(0) as u64
    }

    pub fn advance(&self, seconds: i64) {
        self.offset.fetch_add(seconds, Ordering::Relaxed);
    }
}
