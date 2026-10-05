//! HTTP side of tournaments: who is asking, the revision and idempotency
//! rules, and the response bodies. The bracket itself, its progression and
//! its persistence are the `tournament` domain module's.
mod view;

use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    api::ErrorCode,
    encoding::{Counter, is_prefixed_id},
    json,
    tournament::{CreateTournament, RegisterEntrant},
};
use rusqlite::{Transaction, params};
use serde::{Deserialize, Serialize};

use self::view::snapshot;
use crate::{
    AppState,
    auth::{self, Role},
    ctx::Ctx,
    error::{ApiFailure, Result},
    events::Viewer,
    http::{Body, GENERAL_BODY, expected_revision, idempotency_key, ok},
    routes::{
        matches::{idempotent, into_response, service_viewer, viewer_of},
        policy, records,
    },
    tournament::{self, Applied, Tournament},
};

fn visible(tx: &Transaction<'_>, viewer: &Viewer, tournament: &Tournament) -> Result<bool> {
    Ok(match viewer {
        Viewer::Provider { connection_id, .. } => *connection_id == tournament.connection_id,
        Viewer::Organizer { tenant_id } => *tenant_id == tournament.tenant_id,
        Viewer::Player { ember_id } => tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM tournament_entrants WHERE tournament_id = ?1 AND ember_id = ?2)",
            params![tournament.id, ember_id.as_str()],
            |row| row.get(0),
        )?,
    })
}

fn stale(current: u64) -> ApiFailure {
    ApiFailure::new(
        ErrorCode::StaleRevision,
        "The tournament changed. Refresh it and try again.",
    )
    .detail("current_revision", current.to_string())
}

fn check_reason(reason: &str) -> Result<()> {
    if reason.trim().is_empty() || reason.len() > 512 || reason.chars().any(char::is_control) {
        return Err(ApiFailure::invalid(
            "A reason of 1 to 512 bytes is required.",
        ));
    }
    Ok(())
}

fn actor(role: Role) -> &'static str {
    match role {
        Role::Provider => "service",
        Role::Organizer => "organizer",
    }
}

/// The answer to a request that may only repeat an earlier one: 201 when it
/// changed something, 200 when it found the work already done.
fn created_or_found(
    tx: &Transaction<'_>,
    applied: Applied,
) -> Result<(StatusCode, serde_json::Value)> {
    let (status, tournament) = match applied {
        Applied::Unchanged(tournament) => (StatusCode::OK, tournament),
        Applied::Changed(tournament) => (StatusCode::CREATED, tournament),
    };
    Ok((status, snapshot(tx, &tournament)?))
}

/// `POST /v1/tournaments`: a provider opens registration on its connection.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    policy::generic_connection(&state, &connection_id).await?;
    let key = idempotency_key(&headers)?;
    let command: CreateTournament = body.parse()?;
    command.check()?;
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let tenant_id = service.tenant_id.clone();
    let result = state
        .db
        .write(move |tx| {
            idempotent(
                tx,
                &service.id,
                "/v1/tournaments",
                &key,
                &digest,
                ctx.now,
                |tx| {
                    let applied = tournament::create(
                        tx,
                        &ctx,
                        &tenant_id,
                        &connection_id,
                        &command,
                        &digest,
                    )?;
                    created_or_found(tx, applied)
                },
            )
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}

/// `GET /v1/tournaments/{id}`: its provider, an organizer of its tenant, or
/// one of its entrants.
pub async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let viewer = viewer_of(&auth::any(&state, &headers).await?)?;
    let body = state
        .db
        .read(move |tx| {
            let tournament = tournament::load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &tournament)? {
                return Err(ApiFailure::not_found());
            }
            let mut body = snapshot(tx, &tournament)?;
            if let Viewer::Player { ember_id } = &viewer {
                records::redact(&mut body, ember_id);
            }
            Ok(body)
        })
        .await?;
    Ok(ok(&body))
}

/// `POST /v1/tournaments/{id}/entrants`: the provider registers a linked
/// player who asked to enter, before the tournament starts. Registering again
/// changes nothing.
pub async fn register(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let key = idempotency_key(&headers)?;
    let command: RegisterEntrant = body.parse()?;
    command.check()?;
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/tournaments/{id}/entrants");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let tournament = tournament::load(tx, &id)?
                    .filter(|tournament| tournament.connection_id == connection_id)
                    .ok_or_else(ApiFailure::not_found)?;
                let applied = tournament::register(tx, &ctx, tournament, &command)?;
                created_or_found(tx, applied)
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct WithdrawCommand {
    #[serde(default)]
    expected_revision: Option<Counter>,
}

/// `POST /v1/tournaments/{id}/entrants/{participant_id}/withdraw`: its
/// provider or an organizer of its tenant, at the player's request or for a
/// no-show. Once running, every set they have left goes to their opponent.
pub async fn withdraw_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((id, participant)): Path<(String, String)>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let viewer = service_viewer(&service)?;
    let key = idempotency_key(&headers)?;
    let command: WithdrawCommand = if body.0.is_empty() {
        WithdrawCommand {
            expected_revision: None,
        }
    } else {
        body.parse()?
    };
    if !is_prefixed_id(&participant, "epl") {
        return Err(ApiFailure::invalid("Name the participant_id to withdraw."));
    }
    let expected = command.expected_revision.map(|revision| revision.0);
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/tournaments/{id}/entrants/{participant}/withdraw");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let tournament = tournament::load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
                if !visible(tx, &viewer, &tournament)? {
                    return Err(ApiFailure::not_found());
                }
                if expected.is_some_and(|expected| expected != tournament.revision) {
                    return Err(stale(tournament.revision));
                }
                let tournament = tournament::withdraw_participant(
                    tx,
                    &ctx,
                    &tournament,
                    &participant,
                    (actor(service.role), service.id.clone()),
                )?;
                Ok((StatusCode::OK, snapshot(tx, &tournament)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct StartCommand {
    #[serde(default)]
    expected_revision: Option<Counter>,
    /// Every registered `participant_id`, top seed first. Registration order
    /// when absent.
    #[serde(default)]
    seeding: Option<Vec<String>>,
}

/// `POST /v1/tournaments/{id}/start`: its provider or an organizer of its
/// tenant closes registration, seeds the entrants and starts the first sets.
pub async fn start(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let viewer = service_viewer(&service)?;
    let key = idempotency_key(&headers)?;
    let command: StartCommand = body.parse()?;
    let expected = expected_revision(
        &headers,
        command.expected_revision.map(|revision| revision.0),
    )?;
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/tournaments/{id}/start");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let tournament = tournament::load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
                if !visible(tx, &viewer, &tournament)? {
                    return Err(ApiFailure::not_found());
                }
                if tournament.revision != expected {
                    return Err(stale(tournament.revision));
                }
                let tournament = tournament::start(
                    tx,
                    &ctx,
                    &tournament,
                    command.seeding.as_deref(),
                    (actor(service.role), service.id.clone()),
                )?;
                Ok((StatusCode::OK, snapshot(tx, &tournament)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct CancelCommand {
    reason: String,
    #[serde(default)]
    expected_revision: Option<Counter>,
}

/// `POST /v1/tournaments/{id}/cancel`: its provider or an organizer of its
/// tenant. Sets being played have their matches cancelled.
pub async fn cancel(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let viewer = service_viewer(&service)?;
    let key = idempotency_key(&headers)?;
    let command: CancelCommand = body.parse()?;
    check_reason(&command.reason)?;
    let expected = expected_revision(
        &headers,
        command.expected_revision.map(|revision| revision.0),
    )?;
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/tournaments/{id}/cancel");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let tournament = tournament::load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
                if !visible(tx, &viewer, &tournament)? {
                    return Err(ApiFailure::not_found());
                }
                if tournament.revision != expected {
                    return Err(stale(tournament.revision));
                }
                let tournament = tournament::cancel(
                    tx,
                    &ctx,
                    &tournament,
                    &command.reason,
                    (actor(service.role), service.id.clone()),
                )?;
                Ok((StatusCode::OK, snapshot(tx, &tournament)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}
