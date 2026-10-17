//! How the bridge refuses a BluMint match creation or close: BluMint reads
//! only a `code` at the top level of the body (plus `players` where the code
//! names players) and files any other 400 or 409 as an answer it cannot read,
//! which is final. So these routes answer in BluMint's registry format
//! instead of the bridge's `{ "error": ... }` envelope. A refusal is an `Err`, so
//! `matches::idempotent` never stores it against the key.
//!
//! The bridge's own failures (a database it cannot reach, a credential it
//! does not know) keep the envelope: BluMint reads a 503 as an outage and
//! retries it, whatever the body.
use axum::{
    http::StatusCode,
    response::{IntoResponse, Response},
};
use ember_protocol::{EmberId, api::ErrorCode};
use serde_json::json;

use crate::error::ApiFailure;

/// Why a creation or close was refused, as BluMint's registry names it, or
/// the bridge's own failure.
pub enum BluMintRefusal {
    /// The body is not the JSON BluMint documents, or breaks its rules.
    InvalidRequest,
    /// A match Ember cannot play: not two teams of one, or a set too long.
    MatchUnsupported,
    /// The `Idempotency-Key` already names a different creation.
    KeyReused,
    /// Players with no current link on the connection, as BluMint sent them.
    PlayerUnlinked(Vec<EmberId>),
    /// Players still in another match on the connection. BluMint asks again
    /// under the same key until they are free.
    PlayerBusy(Vec<EmberId>),
    /// Not a refusal: the bridge's own failure, in its own envelope.
    Bridge(ApiFailure),
}

impl From<ApiFailure> for BluMintRefusal {
    fn from(failure: ApiFailure) -> Self {
        match failure.code {
            ErrorCode::InvalidRequest => Self::InvalidRequest,
            ErrorCode::IdempotencyConflict => Self::KeyReused,
            _ => Self::Bridge(failure),
        }
    }
}

impl From<rusqlite::Error> for BluMintRefusal {
    fn from(error: rusqlite::Error) -> Self {
        Self::Bridge(error.into())
    }
}

impl IntoResponse for BluMintRefusal {
    fn into_response(self) -> Response {
        let (status, body) = match self {
            Self::InvalidRequest => (
                StatusCode::BAD_REQUEST,
                json!({ "code": "invalid_request" }),
            ),
            Self::MatchUnsupported => (
                StatusCode::BAD_REQUEST,
                json!({ "code": "match_unsupported" }),
            ),
            Self::KeyReused => (StatusCode::CONFLICT, json!({ "code": "key_reused" })),
            Self::PlayerUnlinked(players) => (
                StatusCode::BAD_REQUEST,
                json!({ "code": "player_unlinked", "players": players }),
            ),
            Self::PlayerBusy(players) => (
                StatusCode::LOCKED,
                json!({ "code": "player_busy", "players": players }),
            ),
            Self::Bridge(failure) => return failure.into_response(),
        };
        crate::http::json(status, &body)
    }
}

/// Runs a BluMint route's write. A refusal commits as the answer, with
/// whatever the route kept of it (a refused creation's key, a closed key);
/// the bridge's own failure rolls the write back.
pub fn outcome<T>(
    work: impl FnOnce() -> Result<T, BluMintRefusal>,
) -> crate::error::Result<Result<T, BluMintRefusal>> {
    match work() {
        Err(BluMintRefusal::Bridge(failure)) => Err(failure),
        answer => Ok(answer),
    }
}
