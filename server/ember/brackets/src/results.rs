//! The connection's `results_url`. Each delivery is verified before its body
//! is read (Standard Webhooks, as `verifyResult` in the TypeScript SDK does),
//! read once into a typed result, recorded once by its `webhook-id`, and
//! answered 2xx only after that write. The tournament's worker does the
//! rest.
//!
//! A delivery that fails verification is answered 503, which the bridge
//! retries for a day, and logged once until one verifies again: a wrong
//! secret or a drifting clock is the operator's to fix, and a 4xx would make
//! the bridge give the result up for good.
use std::sync::{Mutex, PoisonError};

use axum::{
    body::Bytes,
    extract::State,
    http::{HeaderMap, StatusCode},
};
use ember_protocol::{
    Error, json,
    partner::{MatchResult, RESULT_TYPE, result_id},
    webhook::{self, Headers, Secret},
};

use crate::{Connector, log, sets::Received};

pub(crate) const MAX_BODY: usize = 64 * 1024;

/// What the results URL checks a delivery against.
pub(crate) struct Gate {
    /// The current secret, then the one it replaces during a rotation.
    pub secrets: Vec<Secret>,
    pub connection_id: String,
    /// The refusals logged since a delivery last verified.
    logged: Mutex<Vec<&'static str>>,
}

impl Gate {
    pub fn new(secrets: Vec<Secret>, connection_id: String) -> Self {
        Self {
            secrets,
            connection_id,
            logged: Mutex::default(),
        }
    }

    /// Logs a refusal the first time it happens in a run of them.
    fn refuse(&self, status: StatusCode, why: &'static str) -> StatusCode {
        let mut logged = self.logged.lock().unwrap_or_else(PoisonError::into_inner);
        if !logged.contains(&why) {
            logged.push(why);
            log!("results URL: refused a delivery: {why}");
        }
        status
    }

    fn verified(&self) {
        self.logged
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .clear();
    }

    /// Verifies a delivery and reads it.
    fn check(&self, headers: &Headers, body: &[u8], now: u64) -> Result<Received, StatusCode> {
        let secrets: Vec<&Secret> = self.secrets.iter().collect();
        match webhook::verify(&secrets, headers, body, now) {
            Ok(()) => self.verified(),
            Err(Error::Expired) => {
                return Err(self.refuse(
                    StatusCode::SERVICE_UNAVAILABLE,
                    "its timestamp is more than 5 minutes off this machine's clock",
                ));
            }
            Err(_) => {
                return Err(self.refuse(
                    StatusCode::SERVICE_UNAVAILABLE,
                    "its signature does not match the result secret",
                ));
            }
        }
        // Signed by the bridge from here on: a refusal is permanent, and
        // means the bridge and this connector disagree on the format.
        let result: MatchResult = json::parse_as(body, MAX_BODY)
            .map_err(|_| self.refuse(StatusCode::BAD_REQUEST, "its body is not a match result"))?;
        if result.kind != RESULT_TYPE || headers.id != result_id(&result.match_id) {
            return Err(self.refuse(StatusCode::BAD_REQUEST, "it is not a match result"));
        }
        if result.connection_id != self.connection_id {
            return Err(self.refuse(StatusCode::BAD_REQUEST, "it is for another connection"));
        }
        Received::from_result(&result).ok_or_else(|| {
            self.refuse(
                StatusCode::BAD_REQUEST,
                "its participants or winner do not add up",
            )
        })
    }
}

impl Connector {
    /// Verifies a delivery and records it once. Returns whether it was new.
    pub fn accept(&self, headers: &Headers, body: &[u8]) -> Result<bool, StatusCode> {
        let inner = &self.0;
        let now = inner.clock.now();
        let received = inner.gate.check(headers, body, now)?;
        let desk = inner
            .desk
            .as_ref()
            .map(|desk| desk.brackets())
            .unwrap_or_default();
        let bracket = inner
            .brackets
            .iter()
            .chain(&desk)
            .find(|bracket| bracket.key == received.id.bracket);
        // With the desk on, a result for no bracket here may be for an
        // event whose worker has not started yet: it waits for one.
        let fresh = inner
            .store
            .receive(
                &headers.id,
                &received,
                now,
                bracket.is_none() && inner.desk.is_none(),
            )
            .map_err(|_| StatusCode::SERVICE_UNAVAILABLE)?;
        match bracket {
            Some(bracket) => bracket.wake.notify_one(),
            None if inner.desk.is_some() => {}
            None if fresh => log!(
                "result of Ember match {} ({}) is for no tournament configured here; ignored",
                received.match_id,
                received.id
            ),
            None => {}
        }
        Ok(fresh)
    }
}

pub(crate) async fn receive(
    State(connector): State<Connector>,
    headers: HeaderMap,
    body: Bytes,
) -> StatusCode {
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
    // The write is synced to disk: keep it off the runtime's threads.
    let accepted = tokio::task::spawn_blocking(move || connector.accept(&headers, &body)).await;
    match accepted {
        // A repeat is acknowledged too: it is already recorded.
        Ok(Ok(_)) => StatusCode::NO_CONTENT,
        Ok(Err(status)) => status,
        Err(_) => StatusCode::SERVICE_UNAVAILABLE,
    }
}
