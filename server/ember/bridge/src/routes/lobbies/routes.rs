//! The lobby routes: a provider opens a lobby, queues and removes players
//! at their request, and its provider or organizer closes it.
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    encoding::{Counter, is_prefixed_id},
    event::{Kind, LobbyCreatedData},
    lobby::{CreateLobby, JoinLobby},
};
use rusqlite::{OptionalExtension, params};
use serde::{Deserialize, Serialize};
use serde_json::json;

use super::{
    MAX_PLAYERS,
    model::{load, lobby_event, snapshot, touch, visible},
    rotation::{advance, announce, remove, set_entry},
};
use crate::{
    AppState,
    audit::audit,
    auth::{self, Role},
    error::{ApiFailure, Result},
    events::Viewer,
    http::{Body, GENERAL_BODY, expected_revision, idempotency_key, ok},
    routes::{idempotency::ServiceCall, ledger, policy, records},
};

fn closed() -> ApiFailure {
    ApiFailure::new(ErrorCode::StaleRevision, "The lobby is closed.")
}

/// The lobby moved past the revision the request expected.
fn stale(current: u64) -> ApiFailure {
    ApiFailure::new(
        ErrorCode::StaleRevision,
        "The lobby changed. Refresh it and try again.",
    )
    .detail("current_revision", current.to_string())
}

/// `POST /v1/lobbies`: a provider opens a lobby on its connection.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    policy::generic_connection(&state, &connection_id).await?;
    let key = idempotency_key(&headers)?;
    let command: CreateLobby = body.parse()?;
    command.check()?;
    let tenant_id = service.tenant_id.clone();
    ServiceCall::new("/v1/lobbies")
        .run(&state, service.id.clone(), key, command, move |tx, ctx, digest, command| {
            let existing: Option<(String, String)> = tx
                .query_row(
                    "SELECT id, create_digest FROM lobbies WHERE tenant_id = ?1 AND connection_id = ?2 AND external_lobby_id = ?3",
                    params![tenant_id, connection_id, command.external_lobby_id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?;
            if let Some((id, stored)) = existing {
                if stored != digest {
                    return Err(ApiFailure::new(
                        ErrorCode::IdempotencyConflict,
                        "That external_lobby_id already names a different lobby.",
                    ));
                }
                let lobby = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
                return Ok((StatusCode::OK, snapshot(tx, &lobby)?));
            }
            let id = crate::util::new_id("elb");
            tx.execute(
                "INSERT INTO lobbies (id, tenant_id, connection_id, external_lobby_id, create_digest, revision, state,
                    games_to_win, rotation, required_build_id, metadata, current_match_id, sets_started, sets_completed,
                    next_position, streak_holder, streak, created_at, updated_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, 1, 'open', ?6, ?7, ?8, ?9, NULL, 0, 0, 1, NULL, 0, ?10, ?10)",
                params![
                    id,
                    tenant_id,
                    connection_id,
                    command.external_lobby_id,
                    digest,
                    command.games_to_win,
                    command.rotation.as_str(),
                    command.required_build_id,
                    serde_json::to_string(&command.metadata).unwrap_or_default(),
                    ctx.now
                ],
            )?;
            let lobby = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
            lobby_event(
                tx,
                ctx,
                &lobby,
                Kind::LobbyCreated,
                serde_json::to_value(LobbyCreatedData {
                    lobby_id: id.clone(),
                    external_lobby_id: command.external_lobby_id.clone(),
                    lobby_revision: Counter(1),
                    games_to_win: command.games_to_win,
                    rotation: command.rotation,
                    metadata: command.metadata.clone(),
                })
                .unwrap_or_default(),
            )?;
            Ok((StatusCode::CREATED, snapshot(tx, &lobby)?))
        })
        .await
}

/// `GET /v1/lobbies/{id}`: its provider, an organizer of its tenant, or a
/// player who has joined it.
pub async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let viewer = Viewer::of(&auth::any(&state, &headers).await?)?;
    let body = state
        .db
        .read(move |tx| {
            let lobby = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &lobby)? {
                return Err(ApiFailure::not_found());
            }
            let mut body = snapshot(tx, &lobby)?;
            if let Viewer::Player { ember_id } = &viewer {
                records::redact(&mut body, ember_id);
            }
            Ok(body)
        })
        .await?;
    Ok(ok(&body))
}

/// `POST /v1/lobbies/{id}/queue`: the provider adds a player who asked to
/// join. The player must be linked on this connection and not already in
/// another open lobby there. Joining again while queued or seated changes nothing.
pub async fn join(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let key = idempotency_key(&headers)?;
    let command: JoinLobby = body.parse()?;
    command.check()?;
    let path = format!("/v1/lobbies/{id}/queue");
    ServiceCall::new(path)
        .run(&state, service.id.clone(), key, command, move |tx, ctx, _, command| {
            let lobby = load(tx, &id)?.filter(|lobby| lobby.connection_id == connection_id).ok_or_else(ApiFailure::not_found)?;
            if !lobby.open {
                return Err(closed());
            }
            if !ledger::linked(tx, &connection_id, &command.participant_id, &command.ember_id)? {
                return Err(ApiFailure::invalid(
                    "That participant is not currently linked to that Ember ID on this connection.",
                ));
            }
            let current: Option<String> = tx
                .query_row(
                    "SELECT state FROM lobby_entries WHERE lobby_id = ?1 AND ember_id = ?2",
                    params![lobby.id, command.ember_id.as_str()],
                    |row| row.get(0),
                )
                .optional()?;
            if current.is_some_and(|state| state != "left") {
                return Ok((StatusCode::OK, snapshot(tx, &lobby)?));
            }
            let elsewhere: bool = tx.query_row(
                "SELECT EXISTS (SELECT 1 FROM lobby_entries e JOIN lobbies l ON l.id = e.lobby_id
                   WHERE l.connection_id = ?1 AND l.state = 'open' AND l.id != ?2 AND e.ember_id = ?3 AND e.state != 'left')",
                params![connection_id, lobby.id, command.ember_id.as_str()],
                |row| row.get(0),
            )?;
            if elsewhere {
                return Err(ApiFailure::new(ErrorCode::LeaseConflict, "That player is already in another open lobby."));
            }
            let players: i64 = tx.query_row(
                "SELECT COUNT(*) FROM lobby_entries WHERE lobby_id = ?1 AND state != 'left'",
                [&lobby.id],
                |row| row.get(0),
            )?;
            if players >= MAX_PLAYERS {
                return Err(ApiFailure::new(ErrorCode::RateLimited, "The lobby is full.").detail("max_players", MAX_PLAYERS));
            }
            tx.execute(
                "INSERT INTO lobby_entries (lobby_id, ember_id, participant_id, state, position, slot, joined_at, updated_at)
                 VALUES (?1, ?2, ?3, 'left', NULL, NULL, ?4, ?4)
                 ON CONFLICT (lobby_id, ember_id) DO UPDATE SET participant_id = excluded.participant_id",
                params![lobby.id, command.ember_id.as_str(), command.participant_id, ctx.now],
            )?;
            set_entry(tx, &lobby.id, &command.ember_id, "queued", None, ctx.now)?;
            let mut dropped = Vec::new();
            let started = advance(tx, ctx, &lobby.id, &mut dropped)?;
            announce(tx, ctx, &lobby.id, "joined", Some(&command.ember_id), &dropped, started)?;
            let lobby = load(tx, &lobby.id)?.ok_or_else(ApiFailure::unavailable)?;
            Ok((StatusCode::CREATED, snapshot(tx, &lobby)?))
        })
        .await
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct LeaveCommand {
    /// A canonical decimal string, like every revision (spec 9.2).
    #[serde(default)]
    expected_revision: Option<Counter>,
}

/// `POST /v1/lobbies/{id}/queue/{participant_id}/leave`: the provider takes a
/// player out, at their request. Leaving a seat cancels the running set.
pub async fn leave(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((id, participant)): Path<(String, String)>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let key = idempotency_key(&headers)?;
    let command: LeaveCommand = if body.0.is_empty() {
        LeaveCommand {
            expected_revision: None,
        }
    } else {
        body.parse()?
    };
    if !is_prefixed_id(&participant, "epl") {
        return Err(ApiFailure::invalid("Name the participant_id to remove."));
    }
    let expected = command.expected_revision.map(|revision| revision.0);
    let path = format!("/v1/lobbies/{id}/queue/{participant}/leave");
    ServiceCall::new(path)
        .run(&state, service.id.clone(), key, command, move |tx, ctx, _, _| {
            let lobby = load(tx, &id)?.filter(|lobby| lobby.connection_id == connection_id).ok_or_else(ApiFailure::not_found)?;
            if expected.is_some_and(|expected| expected != lobby.revision) {
                return Err(stale(lobby.revision));
            }
            let ember_id: Option<String> = tx
                .query_row(
                    "SELECT ember_id FROM lobby_entries WHERE lobby_id = ?1 AND participant_id = ?2 AND state != 'left'",
                    params![lobby.id, participant],
                    |row| row.get(0),
                )
                .optional()?;
            if let Some(ember_id) = ember_id {
                let ember_id = EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?;
                remove(tx, ctx, &lobby, &ember_id, "left")?;
                audit(tx, ctx.now, ("service", service.id.clone()), "lobby.leave", &lobby.id, "ok", None)?;
            }
            let lobby = load(tx, &lobby.id)?.ok_or_else(ApiFailure::unavailable)?;
            Ok((StatusCode::OK, snapshot(tx, &lobby)?))
        })
        .await
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct CloseCommand {
    reason: String,
    #[serde(default)]
    expected_revision: Option<Counter>,
}

/// `POST /v1/lobbies/{id}/close`: its provider or an organizer of its tenant.
/// A running set is cancelled and everyone leaves.
pub async fn close(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let viewer = Viewer::of_service(&service)?;
    let key = idempotency_key(&headers)?;
    let command: CloseCommand = body.parse()?;
    if command.reason.trim().is_empty()
        || command.reason.len() > 512
        || command.reason.chars().any(char::is_control)
    {
        return Err(ApiFailure::invalid(
            "A reason of 1 to 512 bytes is required.",
        ));
    }
    let expected = expected_revision(
        &headers,
        command.expected_revision.map(|revision| revision.0),
    )?;
    let path = format!("/v1/lobbies/{id}/close");
    let actor = match service.role {
        Role::Provider => "service",
        Role::Organizer => "organizer",
    };
    ServiceCall::new(path)
        .run(&state, service.id.clone(), key, command, move |tx, ctx, _, command| {
            let lobby = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &lobby)? {
                return Err(ApiFailure::not_found());
            }
            if lobby.revision != expected {
                return Err(stale(lobby.revision));
            }
            if !lobby.open {
                return Err(closed());
            }
            if let Some(current) = &lobby.current_match_id {
                ledger::cancel_lobby_set(tx, ctx, current, "lobby_closed")?;
            }
            tx.execute(
                "UPDATE lobbies SET state = 'closed', current_match_id = NULL, streak_holder = NULL, streak = 0 WHERE id = ?1",
                [&lobby.id],
            )?;
            tx.execute(
                "UPDATE lobby_entries SET state = 'left', slot = NULL, position = NULL, updated_at = ?2
                 WHERE lobby_id = ?1 AND state != 'left'",
                params![lobby.id, ctx.now],
            )?;
            let revision = touch(tx, &lobby.id, ctx.now)?;
            let after = load(tx, &lobby.id)?.ok_or_else(ApiFailure::unavailable)?;
            lobby_event(
                tx,
                ctx,
                &after,
                Kind::LobbyClosed,
                json!({ "lobby_id": lobby.id, "lobby_revision": revision.to_string(), "reason": command.reason }),
            )?;
            audit(tx, ctx.now, (actor, service.id.clone()), "lobby.close", &lobby.id, "ok", Some(&command.reason))?;
            Ok((StatusCode::OK, snapshot(tx, &after)?))
        })
        .await
}
