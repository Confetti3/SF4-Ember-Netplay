//! `POST /v1/matches`: a provider creates a match.
use axum::{
    extract::State,
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{api::ErrorCode, matches::CreateMatch, play::play_url};
use rusqlite::{OptionalExtension, Transaction, params};
use serde_json::json;

use crate::{
    AppState, auth,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, idempotency_key},
    routes::{
        idempotency::ServiceCall,
        ledger::{Match, NewMatch, ORGANIZER_PROFILE, insert_match, load},
        lobbies, policy,
    },
    tournament,
};

/// The rules profiles a match may use. `ember-room-v1` games are played in an
/// Ember room under the room's own settings and reported by both fighters.
/// Native profiles such as `usf4-standard-v1` are refused until a tested
/// rules translator exists.
pub const PROFILES: [&str; 2] = [ORGANIZER_PROFILE, ember_protocol::play::PROFILE];

/// `POST /v1/matches`.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    policy::generic_connection(&state, &connection_id).await?;
    let key = idempotency_key(&headers)?;
    let command: CreateMatch = body.parse()?;
    command.check()?;
    if command.external_match_id.starts_with(lobbies::MATCH_PREFIX)
        || command
            .external_match_id
            .starts_with(tournament::MATCH_PREFIX)
    {
        return Err(ApiFailure::invalid(
            "external_match_id values starting with lobby: or tournament: are reserved for lobby and tournament sets.",
        ));
    }
    if !PROFILES.contains(&command.rules.native_rules_profile.as_str()) {
        return Err(ApiFailure::new(
            ErrorCode::UnsupportedRules,
            "That rules profile is not supported. This bridge accepts organizer-reported-v1 and ember-room-v1.",
        )
        .detail("supported_profiles", PROFILES.to_vec()));
    }
    let tenant_id = service.tenant_id.clone();
    ServiceCall::new("/v1/matches")
        .run(
            &state,
            service.id.clone(),
            key,
            command,
            move |tx, ctx, digest, command| {
                create_match(tx, ctx, &tenant_id, &connection_id, &command, digest)
            },
        )
        .await
}

fn create_match(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tenant_id: &str,
    connection_id: &str,
    command: &CreateMatch,
    digest: &str,
) -> Result<(StatusCode, serde_json::Value)> {
    let existing: Option<(String, String)> = tx
        .query_row(
            "SELECT id, create_digest FROM matches WHERE tenant_id = ?1 AND connection_id = ?2 AND external_match_id = ?3",
            params![tenant_id, connection_id, command.external_match_id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?;
    if let Some((id, stored)) = existing {
        // The same logical match created twice is one match (PROVIDER-02).
        if stored != digest {
            return Err(ApiFailure::new(
                ErrorCode::IdempotencyConflict,
                "That external_match_id already names a different match.",
            ));
        }
        let found = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
        return Ok((StatusCode::OK, created_body(ctx, &found)));
    }
    let id = insert_match(
        tx,
        ctx,
        &NewMatch {
            tenant_id,
            connection_id,
            external_match_id: &command.external_match_id,
            digest,
            rules: &command.rules,
            required_build_id: &command.required_build_id,
            metadata: &command.metadata,
            participants: command.by_slot(),
            starting_wins: [0, 0],
            lobby_id: None,
            tournament_id: None,
        },
    )?;
    let found = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
    Ok((StatusCode::CREATED, created_body(ctx, &found)))
}

fn created_body(ctx: &Ctx, found: &Match) -> serde_json::Value {
    json!({
        "match_id": found.id,
        "external_match_id": found.external_match_id,
        "state": found.state,
        "revision": found.revision.to_string(),
        "match_url": format!("{}/v1/matches/{}", ctx.config.origin, found.id),
        "play_url": play_url(&ctx.config.bridge_id, &found.id),
    })
}
