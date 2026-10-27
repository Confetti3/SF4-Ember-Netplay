//! `POST /v1/matches/{id}/cancel`.
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{api::ErrorCode, encoding::Counter};
use serde::{Deserialize, Serialize};
use serde_json::json;

use super::{check_reason, read::visible};
use crate::{
    AppState,
    audit::audit,
    auth,
    error::{ApiFailure, Result},
    events::Viewer,
    http::{Body, GENERAL_BODY, expected_revision, idempotency_key},
    routes::{
        idempotency::ServiceCall,
        ledger::{cancel_and_release, load, stale},
    },
};

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct CancelCommand {
    reason: String,
    /// A canonical decimal string, like every revision (spec 9.2).
    #[serde(default)]
    expected_revision: Option<Counter>,
}

/// `POST /v1/matches/{id}/cancel`: provider or organizer, versioned.
pub async fn cancel(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let viewer = Viewer::of_service(&service)?;
    let key = idempotency_key(&headers)?;
    let command: CancelCommand = body.parse()?;
    check_reason(&command.reason)?;
    let expected = expected_revision(
        &headers,
        command.expected_revision.map(|revision| revision.0),
    )?;
    let path = format!("/v1/matches/{id}/cancel");
    ServiceCall::new(path)
        .run(&state, service.id.clone(), key, command, move |tx, ctx, _, command| {
            let found = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &found)? {
                return Err(ApiFailure::not_found());
            }
            if found.revision != expected {
                return Err(stale(found.revision));
            }
            if found.state.is_terminal() {
                return Err(ApiFailure::new(ErrorCode::StaleRevision, "The match is already finished.")
                    .detail("state", found.state.as_str()));
            }
            if let Some(lobby_id) = &found.lobby_id {
                return Err(ApiFailure::new(
                    ErrorCode::LeaseConflict,
                    "This match is a lobby set. Remove a player from the lobby or close the lobby instead.",
                )
                .detail("lobby_id", lobby_id.clone()));
            }
            if let Some(tournament_id) = &found.tournament_id {
                return Err(ApiFailure::new(
                    ErrorCode::LeaseConflict,
                    "This match is a tournament set. Withdraw an entrant or cancel the tournament instead.",
                )
                .detail("tournament_id", tournament_id.clone()));
            }
            let revision = cancel_and_release(tx, ctx, &found, &command.reason)?;
            audit(tx, ctx.now, ("service", service.id.clone()), "match.cancel", &found.id, "ok", Some(&command.reason))?;
            Ok((StatusCode::OK, json!({ "match_id": found.id, "state": "cancelled", "revision": revision.to_string() })))
        })
        .await
}
