//! The connection's `results_url`: each delivery is verified before its body
//! is read (Standard Webhooks, as `verifyResult` in the TypeScript SDK does),
//! recorded once by its `webhook-id`, and answered 2xx only after that
//! write. The tournament's worker does the rest.
use axum::{
    body::Bytes,
    extract::State,
    http::{HeaderMap, StatusCode},
};
use ember_protocol::{
    json,
    partner::{MatchResult, Outcome, RESULT_TYPE, result_id},
    webhook::{self, Headers},
};

use crate::{Connector, log, sets::ExternalId};

pub(crate) const MAX_BODY: usize = 64 * 1024;

/// The checks that tie a verified delivery to its body and to this
/// connector's connection: the type, `res_<match id>`, two participants in
/// slots 0 and 1, and a winner among them exactly when it completed.
fn belongs(result: &MatchResult, headers: &Headers, connection_id: &str) -> bool {
    let slots: Vec<u8> = result.participants.iter().map(|p| p.slot).collect();
    let winner_valid = match (&result.winner_participant_id, result.outcome) {
        (Some(winner), Outcome::Completed) => result
            .participants
            .iter()
            .any(|participant| participant.participant_id == *winner),
        (None, Outcome::Restart | Outcome::Expired) => true,
        _ => false,
    };
    result.kind == RESULT_TYPE
        && headers.id == result_id(&result.match_id)
        && result.connection_id == connection_id
        && slots == [0, 1]
        && winner_valid
}

impl Connector {
    /// Verifies a delivery and records it once. Returns whether it was new.
    pub fn accept(&self, headers: &Headers, body: &[u8]) -> Result<bool, StatusCode> {
        let inner = &self.0;
        webhook::verify(&[&inner.secret], headers, body, inner.clock.now())
            .map_err(|_| StatusCode::UNAUTHORIZED)?;
        let result: MatchResult =
            json::parse_as(body, MAX_BODY).map_err(|_| StatusCode::BAD_REQUEST)?;
        if !belongs(&result, headers, &inner.connection_id) {
            return Err(StatusCode::BAD_REQUEST);
        }
        let bracket = ExternalId::parse(&result.external_match_id).and_then(|id| {
            inner
                .brackets
                .iter()
                .find(|bracket| bracket.key == id.bracket)
        });
        let fresh = inner
            .store
            .receive(
                &headers.id,
                bracket.map_or("", |bracket| bracket.key.as_str()),
                body,
                inner.clock.now(),
                bracket.is_none(),
            )
            .map_err(|_| StatusCode::SERVICE_UNAVAILABLE)?;
        match bracket {
            Some(bracket) => bracket.wake.notify_one(),
            None if fresh => log!(
                "result of Ember match {} ({}) is for no tournament configured here; ignored",
                result.match_id,
                result.external_match_id
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
    match connector.accept(&headers, &body) {
        // A repeat is acknowledged too: it is already recorded.
        Ok(_) => StatusCode::NO_CONTENT,
        Err(status) => status,
    }
}
