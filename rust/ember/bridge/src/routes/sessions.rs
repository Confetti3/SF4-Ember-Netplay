//! Identity challenges, proofs and player sessions (spec 10).
use axum::{
    extract::State,
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    EmberId, PublicKey,
    api::ErrorCode,
    challenge::{self, Action, Challenge, Expected, Method, ProvenRequest, is_api_path},
    encoding::{b64u, decode_b64u, is_prefixed_id},
    json,
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};

use crate::{
    AppState,
    auth::{self, PLAYER_PREFIX, SESSION_SECS},
    config::Config,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json, ok},
    util::{new_id, new_token, random},
};

pub const SCOPES: &[&str] = &["self:read", "tournament:participate"];
const CHALLENGES_PER_MINUTE: usize = 30;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ChallengeRequest {
    public_key: PublicKey,
    action: Action,
    method: Method,
    path: String,
    request_digest: String,
}

/// `POST /v1/auth/challenges`.
pub async fn issue(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let request: ChallengeRequest = body.parse()?;
    let ember_id = request.public_key.ember_id();
    let now = state.now();
    state
        .rate
        .check(
            &format!("challenge:{ember_id}"),
            CHALLENGES_PER_MINUTE,
            60,
            now,
        )
        .map_err(ApiFailure::rate_limited)?;
    if !route_matches(request.action, request.method, &request.path) {
        return Err(ApiFailure::invalid(
            "That action is not available at that method and path.",
        ));
    }
    decode_b64u::<32>(&request.request_digest, "request_digest")?;
    // Everything except opening a session needs a live session for the same
    // identity before a challenge is even issued (spec 10.1).
    if request.action != Action::SessionCreate {
        let player = auth::player(&state, &headers).await?;
        if player.ember_id != ember_id {
            return Err(ApiFailure::forbidden());
        }
    }
    let issued = Challenge {
        version: challenge::VERSION,
        bridge_id: state.config.bridge_id.clone(),
        audience: state.config.origin.clone(),
        challenge_id: new_id("chl"),
        ember_id: ember_id.clone(),
        action: request.action,
        method: request.method,
        path: request.path,
        request_digest: request.request_digest,
        nonce: b64u(&random::<32>()),
        issued_at: now,
        expires_at: now + challenge::LIFETIME_SECS,
    };
    issued.check_shape(state.config.policy())?;
    let canonical = issued.to_value()?.canonical();
    let stored = canonical.clone();
    let key = request.public_key.to_b64u();
    let id = issued.challenge_id.clone();
    state
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO auth_challenges (id, ember_id, public_key, canonical, expires_at) VALUES (?1, ?2, ?3, ?4, ?5)",
                params![id, ember_id.as_str(), key, stored, issued.expires_at],
            )?;
            Ok(())
        })
        .await?;
    Ok(raw_json(StatusCode::CREATED, canonical))
}

fn raw_json(status: StatusCode, body: Vec<u8>) -> Response {
    use axum::response::IntoResponse;
    (
        status,
        [(axum::http::header::CONTENT_TYPE, "application/json")],
        body,
    )
        .into_response()
}

/// The routes each player action may authorize.
fn route_matches(action: Action, method: Method, path: &str) -> bool {
    if !is_api_path(path) {
        return false;
    }
    let id_between = |prefix: &str, kind: &str, suffix: &str| {
        path.strip_prefix(prefix)
            .and_then(|rest| rest.strip_suffix(suffix))
            .is_some_and(|id| is_prefixed_id(id, kind))
    };
    match (action, method) {
        (Action::SessionCreate, Method::Post) => path == "/v1/sessions",
        (Action::LinkClaim, Method::Post) => path == "/v1/link-claims",
        (Action::LinkCancel, Method::Post) => id_between("/v1/link-claims/", "lkc", "/cancel"),
        (Action::LinkRemove, Method::Delete) => id_between("/v1/links/", "lnk", ""),
        (Action::MatchClaim, Method::Post) => id_between("/v1/matches/", "emt", "/claims"),
        (Action::RoomPublish, Method::Post) => id_between("/v1/matches/", "emt", "/room"),
        (Action::HandoffRedeem, Method::Post) => path == super::handoffs::REDEEM_PATH,
        (Action::AttemptPrepare, Method::Post) => {
            id_between("/v1/matches/", "emt", "/attempts/prepare")
        }
        _ => false,
    }
}

/// Verifies a stored challenge against the received command and consumes it
/// in the caller's transaction (spec 10.1). The challenge is rebuilt from the
/// database; nothing in the request can change what it says.
/// The operation a proof must authorize.
pub struct Target<'a> {
    pub action: Action,
    pub method: Method,
    pub path: &'a str,
}

pub fn consume_proof(
    tx: &Transaction<'_>,
    config: &Config,
    now: u64,
    request: &ProvenRequest,
    target: Target<'_>,
    session: Option<&EmberId>,
) -> Result<(EmberId, PublicKey)> {
    let stored = tx
        .query_row(
            "SELECT public_key, canonical, consumed_at FROM auth_challenges WHERE id = ?1",
            [&request.proof.challenge_id],
            |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, Vec<u8>>(1)?,
                    row.get::<_, Option<u64>>(2)?,
                ))
            },
        )
        .optional()?
        .ok_or_else(|| {
            ApiFailure::new(
                ErrorCode::ChallengeExpired,
                "The challenge has expired. Request a new one.",
            )
        })?;
    if stored.2.is_some() {
        return Err(ApiFailure::new(
            ErrorCode::ChallengeUsed,
            "The challenge was already used.",
        ));
    }
    let challenge: Challenge =
        json::parse_as(&stored.1, PROOF_BODY).map_err(|_| ApiFailure::unavailable())?;
    let key = PublicKey::from_b64u(&stored.0).map_err(|_| ApiFailure::unavailable())?;
    if session.is_some_and(|session| *session != challenge.ember_id) {
        return Err(ApiFailure::forbidden());
    }
    let expected = Expected {
        bridge_id: &config.bridge_id,
        audience: &config.origin,
        ember_id: &challenge.ember_id,
        action: target.action,
        method: target.method,
        path: target.path,
        command: &request.command,
    };
    challenge
        .verify(&expected, &request.proof, &key, now, config.policy())
        .map_err(|error| match error {
            ember_protocol::Error::Expired => ApiFailure::new(
                ErrorCode::ChallengeExpired,
                "The challenge has expired. Request a new one.",
            ),
            _ => ApiFailure::new(
                ErrorCode::InvalidSignature,
                "The proof does not match this request.",
            ),
        })?;
    let consumed = tx.execute(
        "UPDATE auth_challenges SET consumed_at = ?1 WHERE id = ?2 AND consumed_at IS NULL",
        params![now, request.proof.challenge_id],
    )?;
    if consumed != 1 {
        return Err(ApiFailure::new(
            ErrorCode::ChallengeUsed,
            "The challenge was already used.",
        ));
    }
    Ok((challenge.ember_id, key))
}

/// Records an identity the first time it proves possession of its key.
pub fn register_identity(
    tx: &Transaction<'_>,
    ember_id: &EmberId,
    key: &PublicKey,
    now: u64,
) -> Result<()> {
    tx.execute(
        "INSERT OR IGNORE INTO identities (ember_id, public_key, created_at) VALUES (?1, ?2, ?3)",
        params![ember_id.as_str(), key.to_b64u(), now],
    )?;
    Ok(())
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SessionCommand {
    requested_scopes: Vec<String>,
}

#[derive(Serialize)]
struct SessionCreated {
    session_token: String,
    token_type: &'static str,
    ember_id: EmberId,
    scopes: Vec<String>,
    expires_at: u64,
}

/// `POST /v1/sessions`.
pub async fn create(State(state): State<AppState>, body: Body<PROOF_BODY>) -> Result<Response> {
    let request = ProvenRequest::parse(&body.0)?;
    let command: SessionCommand = json::from_value(&request.command)?;
    let scopes: Vec<String> = SCOPES
        .iter()
        .filter(|scope| {
            command
                .requested_scopes
                .iter()
                .any(|wanted| wanted == *scope)
        })
        .map(|scope| (*scope).to_owned())
        .collect();
    if scopes.is_empty() || command.requested_scopes.len() > 8 {
        return Err(ApiFailure::invalid("Request at least one supported scope."));
    }
    let config = state.config.clone();
    let keys = state.keys.clone();
    let now = state.now();
    let created = state
        .db
        .write(move |tx| {
            let (ember_id, key) =
                consume_proof(tx, &config, now, &request, Target { action: Action::SessionCreate, method: Method::Post, path: "/v1/sessions" }, None)?;
            register_identity(tx, &ember_id, &key, now)?;
            let token = new_token(PLAYER_PREFIX);
            tx.execute(
                "INSERT INTO sessions (token_hash, ember_id, scopes, created_at, expires_at) VALUES (?1, ?2, ?3, ?4, ?5)",
                params![
                    auth::hash(&keys, "session", &token).as_slice(),
                    ember_id.as_str(),
                    scopes.join(" "),
                    now,
                    now + SESSION_SECS
                ],
            )?;
            Ok(SessionCreated {
                session_token: token,
                token_type: "Bearer",
                ember_id,
                scopes,
                expires_at: now + SESSION_SECS,
            })
        })
        .await?;
    Ok(json(StatusCode::CREATED, &created))
}

/// `DELETE /v1/sessions/current`. Does not unlink or reset anything.
pub async fn revoke(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let now = state.now();
    state
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE sessions SET revoked_at = ?1 WHERE token_hash = ?2",
                params![now, player.token_hash.as_slice()],
            )?;
            Ok(())
        })
        .await?;
    Ok(ok(&serde_json::json!({ "revoked": true })))
}

/// `GET /v1/identity`: the caller's own identity and bridge state only.
pub async fn identity(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let id = player.ember_id.clone();
    let (public_key, created_at, links) = state
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT i.public_key, i.created_at,
                        (SELECT COUNT(*) FROM links l WHERE l.ember_id = i.ember_id AND l.revoked_at IS NULL)
                 FROM identities i WHERE i.ember_id = ?1",
                [id.as_str()],
                |row| Ok((row.get::<_, String>(0)?, row.get::<_, u64>(1)?, row.get::<_, u64>(2)?)),
            )?)
        })
        .await?;
    Ok(ok(&serde_json::json!({
        "ember_id": player.ember_id,
        "fingerprint": player.ember_id.fingerprint(),
        "public_key": public_key,
        "registered_at": created_at,
        "active_links": links,
        "session": { "scopes": player.scopes, "expires_at": player.expires_at },
    })))
}

/// Parses a proof-carrying body whose command decodes into `T`.
pub fn proven<T: serde::de::DeserializeOwned>(bytes: &[u8]) -> Result<(ProvenRequest, T)> {
    let request = ProvenRequest::parse(bytes)?;
    let command = json::from_value(&request.command)?;
    Ok((request, command))
}
