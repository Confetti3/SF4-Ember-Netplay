//! Logical matches (spec 13, 16.5, 17).
//!
//! In this release results come only from organizer adjudication: native
//! game reports, permits and room admission are later work packages. Each
//! adjudicated game is an attempt row with exactly one score effect, so
//! retries and duplicate deliveries cannot add a win (SEC-09).
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Response},
};
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    encoding::{Counter, is_prefixed_id},
    event::Kind,
    json,
    matches::{CreateMatch, DeliveryState, MatchCompleted, MatchState, Resolution, Score},
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::{
    AppState,
    auth::{self, Actor, Role, Service},
    error::{ApiFailure, Result},
    events::{self, NewEvent, emit},
    http::{Body, GENERAL_BODY, expected_revision, idempotency_key, ok},
    routes::links::{Ctx, audit},
};

/// The one rules profile this bridge accepts: results are entered by an
/// organizer, and no native rule is enforced by Ember. Native profiles such
/// as `usf4-standard-v1` are refused until a tested translator exists.
pub const ORGANIZER_PROFILE: &str = "organizer-reported-v1";
const MAX_REASON: usize = 512;
const MAX_EVIDENCE: usize = 16;

#[derive(Clone, Debug, Serialize)]
pub struct Participant {
    pub slot: u8,
    pub participant_id: String,
    pub ember_id: EmberId,
}

struct Match {
    id: String,
    tenant_id: String,
    connection_id: String,
    external_match_id: String,
    revision: u64,
    generation: u64,
    state: MatchState,
    games_to_win: u8,
}

fn load(tx: &Transaction<'_>, id: &str) -> Result<Option<Match>> {
    Ok(tx
        .query_row(
            "SELECT id, tenant_id, connection_id, external_match_id, revision, assignment_generation, state, games_to_win
             FROM matches WHERE id = ?1",
            [id],
            |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, String>(3)?,
                    row.get::<_, u64>(4)?,
                    row.get::<_, u64>(5)?,
                    row.get::<_, String>(6)?,
                    row.get::<_, u8>(7)?,
                ))
            },
        )
        .optional()?
        .map(|row| Match {
            id: row.0,
            tenant_id: row.1,
            connection_id: row.2,
            external_match_id: row.3,
            revision: row.4,
            generation: row.5,
            state: MatchState::parse(&row.6).unwrap_or(MatchState::Failed),
            games_to_win: row.7,
        }))
}

fn participants(tx: &Transaction<'_>, id: &str, generation: u64) -> Result<Vec<Participant>> {
    tx.prepare(
        "SELECT slot, participant_id, ember_id FROM match_participants
         WHERE match_id = ?1 AND assignment_generation = ?2 ORDER BY slot",
    )?
    .query_map(params![id, generation], |row| {
        Ok((
            row.get::<_, u8>(0)?,
            row.get::<_, String>(1)?,
            row.get::<_, String>(2)?,
        ))
    })?
    .map(|row| {
        let (slot, participant_id, ember_id) = row?;
        Ok(Participant {
            slot,
            participant_id,
            ember_id: EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?,
        })
    })
    .collect()
}

/// Accepted wins per slot, from the ledger rather than any room score.
fn scores(tx: &Transaction<'_>, id: &str) -> Result<([u8; 2], Vec<String>)> {
    let mut wins = [0u8; 2];
    let mut accepted = Vec::new();
    let rows = tx
        .prepare("SELECT id, outcome FROM attempts WHERE match_id = ?1 AND state = 'accepted' ORDER BY seq")?
        .query_map([id], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?)))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for (attempt, outcome) in rows {
        match outcome.as_str() {
            "p1_win" => wins[0] += 1,
            "p2_win" => wins[1] += 1,
            _ => continue,
        }
        accepted.push(attempt);
    }
    Ok((wins, accepted))
}

/// Who may see a match: its assigned players, its provider connection, or
/// an organizer of its tenant. Anyone else gets `not_found`.
fn visible(tx: &Transaction<'_>, actor: &Viewer, found: &Match) -> Result<bool> {
    Ok(match actor {
        Viewer::Provider { connection_id, .. } => *connection_id == found.connection_id,
        Viewer::Organizer { tenant_id } => *tenant_id == found.tenant_id,
        Viewer::Player { ember_id } => tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM match_participants WHERE match_id = ?1 AND ember_id = ?2)",
            params![found.id, ember_id.as_str()],
            |row| row.get(0),
        )?,
    })
}

use events::Viewer;

pub fn viewer_of(actor: &Actor) -> Result<Viewer> {
    match actor {
        Actor::Player(player) => {
            if !player.scopes.iter().any(|scope| scope == "self:read") {
                return Err(ApiFailure::forbidden());
            }
            Ok(Viewer::Player {
                ember_id: player.ember_id.clone(),
            })
        }
        Actor::Service(service) => service_viewer(service),
        Actor::Browser(_) => Err(ApiFailure::forbidden()),
    }
}

pub fn service_viewer(service: &Service) -> Result<Viewer> {
    Ok(match service.role {
        Role::Provider => Viewer::Provider {
            tenant_id: service.tenant_id.clone(),
            connection_id: service.provider_connection()?.to_owned(),
        },
        Role::Organizer => Viewer::Organizer {
            tenant_id: service.tenant_id.clone(),
        },
    })
}

/// Replays a stored response for a retried request, or records a new one
/// (spec 10.2, 19.1). The key is scoped to the authenticated actor.
pub fn idempotent(
    tx: &Transaction<'_>,
    actor: &str,
    path: &str,
    key: &str,
    digest: &str,
    now: u64,
    work: impl FnOnce(&Transaction<'_>) -> Result<(StatusCode, serde_json::Value)>,
) -> Result<(StatusCode, serde_json::Value)> {
    let stored: Option<(String, u16, Vec<u8>)> = tx
        .query_row(
            "SELECT command_digest, status, response FROM idempotency_records
             WHERE actor = ?1 AND method = 'POST' AND path = ?2 AND idempotency_key = ?3",
            params![actor, path, key],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
        )
        .optional()?;
    if let Some((stored_digest, status, body)) = stored {
        if stored_digest != digest {
            return Err(ApiFailure::new(
                ErrorCode::IdempotencyConflict,
                "This Idempotency-Key was used for a different request.",
            ));
        }
        let body = serde_json::from_slice(&body).map_err(|_| ApiFailure::unavailable())?;
        return Ok((StatusCode::from_u16(status).unwrap_or(StatusCode::OK), body));
    }
    let (status, body) = work(tx)?;
    tx.execute(
        "INSERT INTO idempotency_records (actor, method, path, idempotency_key, command_digest, status, response, created_at)
         VALUES (?1, 'POST', ?2, ?3, ?4, ?5, ?6, ?7)",
        params![actor, path, key, digest, status.as_u16(), serde_json::to_vec(&body).unwrap_or_default(), now],
    )?;
    Ok((status, body))
}

fn respond((status, body): (StatusCode, serde_json::Value)) -> Response {
    crate::http::json(status, &body)
}

fn stale(current: u64) -> ApiFailure {
    ApiFailure::new(
        ErrorCode::StaleRevision,
        "The match changed. Refresh it and try again.",
    )
    .detail("current_revision", current.to_string())
}

fn bump(tx: &Transaction<'_>, found: &Match, state: MatchState, now: u64) -> Result<u64> {
    let revision = found.revision + 1;
    let changed = tx.execute(
        "UPDATE matches SET revision = ?1, state = ?2, updated_at = ?3 WHERE id = ?4 AND revision = ?5",
        params![revision, state.as_str(), now, found.id, found.revision],
    )?;
    if changed != 1 {
        return Err(stale(found.revision));
    }
    Ok(revision)
}

fn match_event(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    found: &Match,
    kind: Kind,
    data: serde_json::Value,
) -> Result<i64> {
    emit(
        tx,
        &ctx.config,
        ctx.now,
        NewEvent {
            kind,
            tenant_id: &found.tenant_id,
            connection_id: Some(&found.connection_id),
            subject: format!("matches/{}", found.id),
            match_id: Some(&found.id),
            ember_id: None,
            data,
        },
    )
}

/// `POST /v1/matches`.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let key = idempotency_key(&headers)?;
    let command: CreateMatch = body.parse()?;
    command.check()?;
    if command.rules.native_rules_profile != ORGANIZER_PROFILE {
        return Err(ApiFailure::new(
            ErrorCode::UnsupportedRules,
            "That rules profile is not supported. This bridge accepts organizer-reported-v1 only.",
        )
        .detail("supported_profiles", vec![ORGANIZER_PROFILE]));
    }
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let tenant_id = service.tenant_id.clone();
    let result = state
        .db
        .write(move |tx| {
            idempotent(
                tx,
                &service.id,
                "/v1/matches",
                &key,
                &digest,
                ctx.now,
                |tx| create_match(tx, &ctx, &tenant_id, &connection_id, &command, &digest),
            )
        })
        .await?;
    state.committed();
    Ok(respond(result))
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
    for participant in command.by_slot() {
        let linked: bool = tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM external_accounts a JOIN links l ON l.account_id = a.id
              WHERE a.connection_id = ?1 AND a.participant_id = ?2 AND l.ember_id = ?3 AND l.revoked_at IS NULL)",
            params![connection_id, participant.participant_id, participant.ember_id.as_str()],
            |row| row.get(0),
        )?;
        if !linked {
            return Err(ApiFailure::invalid(
                "A participant is not currently linked to that Ember ID on this connection.",
            )
            .detail("slot", participant.slot));
        }
    }
    for participant in command.by_slot() {
        let busy: bool = tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM matches m JOIN match_participants p
               ON p.match_id = m.id AND p.assignment_generation = m.assignment_generation
              WHERE m.connection_id = ?1 AND p.ember_id = ?2
                AND m.state NOT IN ('completed', 'cancelled', 'failed'))",
            params![connection_id, participant.ember_id.as_str()],
            |row| row.get(0),
        )?;
        if busy {
            return Err(ApiFailure::new(
                ErrorCode::LeaseConflict,
                "A participant already has an active match.",
            )
            .detail("slot", participant.slot));
        }
    }
    let id = crate::util::new_id("emt");
    tx.execute(
        "INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
            assignment_generation, state, games_to_win, rules, required_build_id, metadata, delivery_state,
            created_at, updated_at)
         VALUES (?1, ?2, ?3, ?4, ?5, 1, 1, 'awaiting_players', ?6, ?7, ?8, ?9, 'not_required', ?10, ?10)",
        params![
            id,
            tenant_id,
            connection_id,
            command.external_match_id,
            digest,
            command.rules.games_to_win,
            serde_json::to_string(&command.rules).unwrap_or_default(),
            command.required_build_id,
            serde_json::to_string(&command.metadata).unwrap_or_default(),
            ctx.now
        ],
    )?;
    for participant in command.by_slot() {
        tx.execute(
            "INSERT INTO match_participants (match_id, assignment_generation, slot, participant_id, ember_id)
             VALUES (?1, 1, ?2, ?3, ?4)",
            params![id, participant.slot, participant.participant_id, participant.ember_id.as_str()],
        )?;
    }
    let found = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
    let roster = participants(tx, &id, 1)?;
    match_event(
        tx,
        ctx,
        &found,
        Kind::MatchCreated,
        json!({
            "match_id": id,
            "external_match_id": command.external_match_id,
            "match_revision": "1",
            "assignment_generation": "1",
            "state": MatchState::AwaitingPlayers,
            "games_to_win": command.rules.games_to_win,
            "participants": roster,
            "metadata": command.metadata,
        }),
    )?;
    Ok((StatusCode::CREATED, created_body(ctx, &found)))
}

fn created_body(ctx: &Ctx, found: &Match) -> serde_json::Value {
    json!({
        "match_id": found.id,
        "external_match_id": found.external_match_id,
        "state": found.state,
        "revision": found.revision.to_string(),
        "match_url": format!("{}/v1/matches/{}", ctx.config.origin, found.id),
    })
}

/// A consistent snapshot plus the event cursor from the same read (17.3).
fn snapshot(tx: &Transaction<'_>, found: &Match) -> Result<serde_json::Value> {
    let roster = participants(tx, &found.id, found.generation)?;
    let (wins, _) = scores(tx, &found.id)?;
    let (rules, metadata, delivery): (String, String, String) = tx.query_row(
        "SELECT rules, metadata, delivery_state FROM matches WHERE id = ?1",
        [&found.id],
        |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
    )?;
    let attempts = tx
        .prepare(
            "SELECT id, seq, outcome, state, source, adjudication_id FROM attempts WHERE match_id = ?1 ORDER BY seq",
        )?
        .query_map([&found.id], |row| {
            Ok(json!({
                "attempt_id": row.get::<_, String>(0)?,
                "seq": row.get::<_, u64>(1)?,
                "outcome": row.get::<_, String>(2)?,
                "state": row.get::<_, String>(3)?,
                "source": row.get::<_, String>(4)?,
                "adjudication_id": row.get::<_, String>(5)?,
            }))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let scores: Vec<_> = roster
        .iter()
        .map(|p| json!({ "ember_id": p.ember_id, "wins": wins[usize::from(p.slot)] }))
        .collect();
    Ok(json!({
        "match_id": found.id,
        "external_match_id": found.external_match_id,
        "state": found.state,
        "revision": found.revision.to_string(),
        "assignment_generation": found.generation.to_string(),
        "games_to_win": found.games_to_win,
        "rules": serde_json::from_str::<serde_json::Value>(&rules).unwrap_or_default(),
        "participants": roster,
        "scores": scores,
        "attempts": attempts,
        "provider_delivery_state": delivery,
        "metadata": serde_json::from_str::<serde_json::Value>(&metadata).unwrap_or_default(),
        "event_cursor": events::head(tx)?.to_string(),
    }))
}

/// `GET /v1/matches/{id}`.
pub async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let viewer = viewer_of(&auth::any(&state, &headers).await?)?;
    let body = state
        .db
        .read(move |tx| {
            let found = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &found)? {
                return Err(ApiFailure::not_found());
            }
            snapshot(tx, &found)
        })
        .await?;
    Ok(ok(&body))
}

/// `GET /v1/assignments`: the caller's own matches only.
pub async fn assignments(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let config = state.config.clone();
    let body = state
        .db
        .read(move |tx| {
            let rows = tx
                .prepare(
                    "SELECT m.id, m.connection_id, m.state, m.revision, p.slot FROM matches m
                     JOIN match_participants p ON p.match_id = m.id AND p.assignment_generation = m.assignment_generation
                     WHERE p.ember_id = ?1 ORDER BY (m.state IN ('completed', 'cancelled', 'failed')), m.updated_at DESC
                     LIMIT 50",
                )?
                .query_map([player.ember_id.as_str()], |row| {
                    let connection: String = row.get(1)?;
                    Ok(json!({
                        "match_id": row.get::<_, String>(0)?,
                        "provider": config.connection(&connection).map(|(_, c)| c.display_name.clone()),
                        "state": row.get::<_, String>(2)?,
                        "revision": row.get::<_, u64>(3)?.to_string(),
                        "slot": row.get::<_, u8>(4)?,
                    }))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            Ok(json!({ "assignments": rows, "event_cursor": events::head(tx)?.to_string() }))
        })
        .await?;
    Ok(ok(&body))
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct CancelCommand {
    reason: String,
    #[serde(default)]
    expected_revision: Option<u64>,
}

/// `POST /v1/matches/{id}/cancel`: provider or organizer, versioned.
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
    let expected = expected_revision(&headers, command.expected_revision)?;
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/matches/{id}/cancel");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
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
                let revision = bump(tx, &found, MatchState::Cancelled, ctx.now)?;
                match_event(
                    tx,
                    &ctx,
                    &found,
                    Kind::MatchCancelled,
                    json!({ "match_id": found.id, "match_revision": revision.to_string(), "state": "cancelled", "reason": command.reason }),
                )?;
                audit(tx, ctx.now, ("service", service.id.clone()), "match.cancel", &found.id, "ok", Some(&command.reason))?;
                Ok((StatusCode::OK, json!({ "match_id": found.id, "state": "cancelled", "revision": revision.to_string() })))
            })
        })
        .await?;
    state.committed();
    Ok(respond(result))
}

fn check_reason(reason: &str) -> Result<()> {
    if reason.trim().is_empty()
        || reason.len() > MAX_REASON
        || reason.chars().any(|c| c.is_control() && c != '\n')
    {
        return Err(ApiFailure::invalid(
            "A reason of 1 to 512 bytes is required.",
        ));
    }
    Ok(())
}

#[derive(Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
enum AdjudicationKind {
    /// Records one game's outcome as decided by the organizer.
    GameResult,
    /// Voids an accepted game. On a completed match this is a correction.
    VoidGame,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct AdjudicationCommand {
    kind: AdjudicationKind,
    /// 0 or 1 for a win; omitted for a draw.
    #[serde(default)]
    winner_slot: Option<u8>,
    #[serde(default)]
    draw: bool,
    #[serde(default)]
    attempt_id: Option<String>,
    reason: String,
    #[serde(default)]
    evidence: Vec<String>,
    #[serde(default)]
    expected_revision: Option<u64>,
}

/// `POST /v1/matches/{id}/adjudications`: organizers only (SEC-07).
pub async fn adjudicate(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    service.require(Role::Organizer)?;
    let viewer = service_viewer(&service)?;
    let key = idempotency_key(&headers)?;
    let command: AdjudicationCommand = body.parse()?;
    check_reason(&command.reason)?;
    if command.evidence.len() > MAX_EVIDENCE
        || command
            .evidence
            .iter()
            .any(|item| item.is_empty() || item.len() > 256)
    {
        return Err(ApiFailure::invalid(
            "Evidence is up to 16 references of 1 to 256 bytes.",
        ));
    }
    let expected = expected_revision(&headers, command.expected_revision)?;
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/matches/{id}/adjudications");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let found = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
                if !visible(tx, &viewer, &found)? {
                    return Err(ApiFailure::not_found());
                }
                if found.revision != expected {
                    return Err(stale(found.revision));
                }
                apply_adjudication(tx, &ctx, &service, &found, &command, expected)
            })
        })
        .await?;
    state.committed();
    Ok(respond(result))
}

fn apply_adjudication(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    service: &Service,
    found: &Match,
    command: &AdjudicationCommand,
    expected: u64,
) -> Result<(StatusCode, serde_json::Value)> {
    let roster = participants(tx, &found.id, found.generation)?;
    let adjudication_id = crate::util::new_id("adj");
    let (wins_before, _) = scores(tx, &found.id)?;
    let mut events = Vec::new();
    let correcting = found.state == MatchState::Completed;
    match command.kind {
        AdjudicationKind::GameResult => {
            if found.state.is_terminal() {
                return Err(ApiFailure::new(
                    ErrorCode::StaleRevision,
                    "The match is finished. Void a game to correct it first.",
                ));
            }
            let outcome = match (command.winner_slot, command.draw) {
                (Some(0), false) => "p1_win",
                (Some(1), false) => "p2_win",
                (None, true) => "draw",
                _ => {
                    return Err(ApiFailure::invalid(
                        "Give winner_slot 0 or 1, or draw: true.",
                    ));
                }
            };
            if command.attempt_id.is_some() {
                return Err(ApiFailure::invalid(
                    "A game result creates its own attempt.",
                ));
            }
            record_adjudication(tx, ctx, service, found, &adjudication_id, command, expected)?;
            let seq: u64 = tx.query_row(
                "SELECT COALESCE(MAX(seq), 0) + 1 FROM attempts WHERE match_id = ?1",
                [&found.id],
                |row| row.get(0),
            )?;
            let attempt_id = crate::util::new_id("ega");
            tx.execute(
                "INSERT INTO attempts (id, match_id, assignment_generation, seq, outcome, source, state, adjudication_id, created_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, 'organizer_adjudication', 'accepted', ?6, ?7)",
                params![attempt_id, found.id, found.generation, seq, outcome, adjudication_id, ctx.now],
            )?;
            let winner = command
                .winner_slot
                .and_then(|slot| roster.iter().find(|p| p.slot == slot))
                .map(|p| p.ember_id.clone());
            events.push((
                Kind::GameConfirmed,
                json!({
                    "match_id": found.id,
                    "attempt_id": attempt_id,
                    "seq": seq,
                    "outcome": outcome,
                    "winner_id": winner,
                    "resolution": Resolution::OrganizerAdjudication,
                    "adjudication_id": adjudication_id,
                }),
            ));
        }
        AdjudicationKind::VoidGame => {
            let attempt = command
                .attempt_id
                .as_deref()
                .filter(|id| is_prefixed_id(id, "ega"))
                .ok_or_else(|| ApiFailure::invalid("Name the attempt_id to void."))?;
            if command.winner_slot.is_some() || command.draw {
                return Err(ApiFailure::invalid("Voiding a game takes no outcome."));
            }
            if found.state.is_terminal() && !correcting {
                return Err(ApiFailure::new(
                    ErrorCode::StaleRevision,
                    "A cancelled or failed match cannot change.",
                ));
            }
            record_adjudication(tx, ctx, service, found, &adjudication_id, command, expected)?;
            let changed = tx.execute(
                "UPDATE attempts SET state = 'voided', voided_by = ?1 WHERE id = ?2 AND match_id = ?3 AND state = 'accepted'",
                params![adjudication_id, attempt, found.id],
            )?;
            if changed != 1 {
                return Err(ApiFailure::invalid(
                    "That attempt is not an accepted game of this match.",
                ));
            }
        }
    }
    let (wins, accepted) = scores(tx, &found.id)?;
    let n = found.games_to_win;
    let winner_slot = (0..2).find(|&slot| wins[slot] >= n);
    let next = if winner_slot.is_some() {
        MatchState::Completed
    } else {
        MatchState::BetweenGames
    };
    let revision = bump(tx, found, next, ctx.now)?;
    let revision_text = revision.to_string();
    for (kind, mut data) in events {
        data["match_revision"] = revision_text.clone().into();
        match_event(tx, ctx, found, kind, data)?;
    }
    let score_rows: Vec<Score> = roster
        .iter()
        .map(|p| Score {
            ember_id: p.ember_id.clone(),
            wins: wins[usize::from(p.slot)],
        })
        .collect();
    if wins != wins_before {
        match_event(
            tx,
            ctx,
            found,
            Kind::ScoreChanged,
            json!({ "match_id": found.id, "match_revision": revision_text, "games_to_win": n, "scores": score_rows }),
        )?;
    }
    if correcting {
        match_event(
            tx,
            ctx,
            found,
            Kind::MatchCorrected,
            json!({
                "match_id": found.id,
                "match_revision": revision_text,
                "state": next,
                "adjudication_id": adjudication_id,
                "reason": command.reason,
            }),
        )?;
    }
    if let Some(slot) = winner_slot {
        let completed = MatchCompleted {
            match_id: found.id.clone(),
            match_revision: Counter(revision),
            assignment_generation: Counter(found.generation),
            state: MatchState::Completed,
            games_to_win: n,
            scores: score_rows.clone(),
            winner_id: roster[slot].ember_id.clone(),
            resolution: Resolution::OrganizerAdjudication,
            accepted_attempt_ids: accepted,
            evidence_report_ids: Vec::new(),
            provider_delivery_state: DeliveryState::NotRequired,
        };
        completed.check().map_err(|_| ApiFailure::unavailable())?;
        match_event(
            tx,
            ctx,
            found,
            Kind::MatchCompleted,
            serde_json::to_value(&completed).unwrap_or_default(),
        )?;
    }
    Ok((
        StatusCode::CREATED,
        json!({
            "adjudication_id": adjudication_id,
            "match_id": found.id,
            "state": next,
            "revision": revision_text,
            "scores": score_rows,
        }),
    ))
}

fn record_adjudication(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    service: &Service,
    found: &Match,
    id: &str,
    command: &AdjudicationCommand,
    expected: u64,
) -> Result<()> {
    tx.execute(
        "INSERT INTO adjudications (id, match_id, actor, kind, reason, evidence, expected_revision, result, created_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        params![
            id,
            found.id,
            service.id,
            serde_json::to_value(&command.kind).ok().and_then(|v| v.as_str().map(str::to_owned)),
            command.reason,
            serde_json::to_string(&command.evidence).unwrap_or_default(),
            expected,
            serde_json::to_string(&json!({ "winner_slot": command.winner_slot, "draw": command.draw, "attempt_id": command.attempt_id }))
                .unwrap_or_default(),
            ctx.now
        ],
    )?;
    audit(
        tx,
        ctx.now,
        ("organizer", service.id.clone()),
        "match.adjudicate",
        &found.id,
        "ok",
        Some(&command.reason),
    )
}

/// An unlinked identity cannot keep playing an assigned match: every active
/// match on that connection that names it goes to review (spec 11.6).
pub fn on_unlink(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    _tenant: &str,
    connection_id: &str,
    ember_id: &EmberId,
) -> Result<()> {
    let ids = tx
        .prepare(
            "SELECT m.id FROM matches m JOIN match_participants p
               ON p.match_id = m.id AND p.assignment_generation = m.assignment_generation
             WHERE m.connection_id = ?1 AND p.ember_id = ?2
               AND m.state NOT IN ('completed', 'cancelled', 'failed', 'needs_review')",
        )?
        .query_map(params![connection_id, ember_id.as_str()], |row| {
            row.get::<_, String>(0)
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for id in ids {
        let found = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
        let revision = bump(tx, &found, MatchState::NeedsReview, ctx.now)?;
        match_event(
            tx,
            ctx,
            &found,
            Kind::NeedsReview,
            json!({ "match_id": id, "match_revision": revision.to_string(), "state": "needs_review", "reason": "identity_unlinked" }),
        )?;
    }
    Ok(())
}

/// Any response type, for routes that share the idempotency helper.
pub fn into_response(result: (StatusCode, serde_json::Value)) -> Response {
    respond(result).into_response()
}
