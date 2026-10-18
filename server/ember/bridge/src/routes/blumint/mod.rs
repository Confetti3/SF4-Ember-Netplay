//! BluMint's game partner API (2.0.0), for connections of kind `blumint`.
//! BluMint calls three endpoints here, with the connection's provider
//! credential as `Authorization: Bearer`:
//!
//! - player lookup: the sign-in IDs of a BluMint user, answered with the Ember
//!   IDs connected to those Discord accounts (`discord` is the one sign-in
//!   method Ember supports);
//! - match creation (`create`): two teams of one Ember ID each, under an
//!   `Idempotency-Key`, answered with the match ID and its Play link, which
//!   only those two players can use, or refused in BluMint's format
//!   (`refusal`);
//! - match close (`close`): BluMint no longer needs the match its creation's
//!   `Idempotency-Key` made (`keys`), which ends it.
//!
//! The bridge reports each match to BluMint once BluMint's API key for the
//! connection is configured (`report`): its status each time a player
//! arrives or leaves, a game starts, or a game ends with another to come
//! (`delivery::status`), its score
//! after each game that does not end it (`delivery::progress`), then its
//! result, or an abort for a match that ended without one
//! (`delivery::results`). A match whose result the two games disputed is
//! cancelled where it would wait for review (`matches::needs_review`), since
//! BluMint has no organizer review, and its abort lets BluMint ask for the
//! set again. A match that expires (`expiry::expire_stale`), a lifetime
//! after its last game or 24 hours after its creation, is sent as an abort
//! BluMint must not retry, so its host takes the match.
//! `register` tells BluMint where the three endpoints are.
mod close;
mod create;
pub mod keys;
pub mod liveness;
mod refusal;
mod report;

use std::collections::BTreeMap;

use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::EmberId;
use rusqlite::params;
use serde_json::{Value, json};

pub use close::{close, end_unkeyed};
pub use create::create;
pub use refusal::BluMintRefusal;
use refusal::outcome;
pub use report::{progress, status, status_report, submission, unopened_status};

use crate::{
    AppState, auth,
    config::{BLUMINT, Connection},
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    linking::Site,
    routes::policy,
    util::outbound_client,
};

/// The three callbacks BluMint calls, as `register` tells BluMint them.
pub const LOOKUP_PATH: &str = "/v1/blumint/lookup";
pub const MATCHES_PATH: &str = "/v1/blumint/matches";
pub const CLOSE_PATH: &str = "/v1/blumint/matches/close";

/// BluMint's API for a connection: `api_base`, or by its environment.
pub fn api_base(connection: &Connection) -> &str {
    match (&connection.api_base, connection.environment.as_str()) {
        (Some(base), _) => base,
        (None, "production") => "https://app.blumint.com/api",
        (None, _) => "https://staging.blumint.io/api",
    }
}

/// The largest match creation the bridge takes. BluMint passes the host's
/// free-form `matchSettings` through, such as a long rules text the bridge
/// never reads, so a creation gets four times the general cap. The body is
/// read before the credential is checked, so the cap is also what any
/// caller can make the bridge hold per request, which keeps it well short of
/// a megabyte. A close and a lookup keep the general cap.
pub const CREATE_BODY: usize = 256 * 1024;

/// A creation or close body as the handler sees it. A body over the cap, or
/// cut short, is then refused as `invalid_request` in BluMint's format, after
/// authentication, rather than by the extractor in the bridge's envelope,
/// which BluMint cannot read.
type BluMintBody<const N: usize> = std::result::Result<Body<N>, ApiFailure>;

/// BluMint's bodies as plain JSON: its match settings carry decimals
/// (`"gravity": 1.1`), which the bridge's strict profile refuses. The size cap
/// still applies, and nothing here is signed or stored as sent.
fn parse<T: serde::de::DeserializeOwned, const N: usize>(body: &Body<N>) -> Result<T> {
    serde_json::from_slice(&body.0)
        .map_err(|_| ApiFailure::invalid("The request body is not the JSON BluMint documents."))
}

/// The BluMint connection a provider credential speaks for.
async fn connection(state: &AppState, headers: &HeaderMap) -> Result<String> {
    Ok(caller(state, headers).await?.0)
}

/// The BluMint connection a provider credential speaks for, and the
/// credential's id.
async fn caller(state: &AppState, headers: &HeaderMap) -> Result<(String, String)> {
    let service = auth::service(state, headers).await?;
    let id = service.provider_connection()?.to_owned();
    match state.config.connection(&id) {
        Some((_, connection)) if connection.kind == BLUMINT => Ok((id, service.id)),
        _ => Err(ApiFailure::not_found()),
    }
}

/// `POST /v1/blumint/lookup`: every sign-in method of one BluMint user. Only
/// `discord` is answered, with `[]` when none of its IDs is connected.
pub async fn lookup(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let connection_id = connection(&state, &headers).await?;
    let request: BTreeMap<String, Value> = parse(&body)?;
    let user_ids: Vec<String> = request
        .get("discord")
        .and_then(Value::as_array)
        .map(|ids| {
            ids.iter()
                .filter_map(Value::as_str)
                .take(32)
                .map(str::to_owned)
                .collect()
        })
        .unwrap_or_default();
    let ctx = Ctx::of(&state);
    let found = state
        .db
        .write(move |tx| {
            let found = Site::Discord.find_and_link(tx, &ctx, &connection_id, &user_ids)?;
            Ok(found
                .into_iter()
                .map(|player| player.ember_id)
                .collect::<Vec<EmberId>>())
        })
        .await?;
    state.committed();
    Ok(ok(&json!({ "discord": found })))
}

/// The label of the credential BluMint calls the bridge with.
const CALLBACK_LABEL: &str = "BluMint callbacks";

/// Tells BluMint where this bridge's three endpoints are, with a new provider
/// credential for BluMint to call them with. The credential goes straight to
/// BluMint and is never shown. BluMint keeps one credential per route, so
/// once every route has the new one, the ones earlier registrations handed
/// over are revoked. A registration that fails partway revokes nothing,
/// since BluMint may hold either credential on each route.
pub async fn register(state: &AppState, connection_id: &str) -> std::result::Result<(), String> {
    let Some((_, connection)) = state
        .config
        .connection(connection_id)
        .filter(|(_, c)| c.kind == BLUMINT && c.enabled)
    else {
        return Err(format!(
            "{connection_id} is not an enabled blumint connection"
        ));
    };
    let key = state
        .integrations
        .api_keys
        .get(connection_id)
        .ok_or_else(|| {
            format!("no BluMint API key for {connection_id} in the integration secrets")
        })?;
    // The connection's record decides, as it does for result posts, checked
    // again before each call so a connection disabled meanwhile stops them.
    let disabled = || format!("{connection_id} is disabled");
    if !policy::stored_enabled(state, connection_id).await {
        return Err(disabled());
    }
    let (credential, token) =
        crate::issue_identified_credential(state, Some(connection_id), None, CALLBACK_LABEL)
            .await
            .map_err(|_| "could not issue the callback credential".to_owned())?;
    let authentication = json!({
        "config": { "location": "header", "key": "Authorization", "valuePrefix": "Bearer " },
        "secret": token,
    });
    let origin = &state.config.origin;
    let client = outbound_client()
        .build()
        .map_err(|error| error.to_string())?;
    let partway = |error: String| {
        format!(
            "{error}; the earlier callback credentials and the new one, {credential}, stay valid until a registration succeeds"
        )
    };
    for (path, field, ours) in [
        (
            "/tournaments/setLookupPlayer",
            "lookupPlayerUrl",
            LOOKUP_PATH,
        ),
        (
            "/tournaments/match/setCreate",
            "createMatchUrl",
            MATCHES_PATH,
        ),
        ("/tournaments/match/setClose", "closeMatchUrl", CLOSE_PATH),
    ] {
        if !policy::stored_enabled(state, connection_id).await {
            return Err(partway(disabled()));
        }
        let body = json!({ field: format!("{origin}{ours}"), "authentication": authentication });
        let response = client
            .post(format!("{}{path}", api_base(connection)))
            .header("x-api-key", key.as_str())
            .header("content-type", "application/json")
            .body(body.to_string())
            .send()
            .await
            .map_err(|error| partway(format!("{path}: {error}")))?;
        if !response.status().is_success() {
            return Err(partway(format!(
                "{path}: BluMint answered {}",
                response.status()
            )));
        }
    }
    let (connection_id, now) = (connection_id.to_owned(), state.now());
    state
        .db
        .write(move |tx| {
            let superseded = tx
                .prepare(
                    "SELECT id FROM service_credentials
                     WHERE connection_id = ?1 AND role = 'provider' AND label = ?2 AND id != ?3 AND revoked_at IS NULL",
                )?
                .query_map(params![connection_id, CALLBACK_LABEL, credential], |row| {
                    row.get::<_, String>(0)
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            for id in superseded {
                crate::revoke_in(tx, &id, now)?;
            }
            Ok(())
        })
        .await
        .map_err(|_| "registered, but could not revoke the earlier callback credentials".to_owned())
}
