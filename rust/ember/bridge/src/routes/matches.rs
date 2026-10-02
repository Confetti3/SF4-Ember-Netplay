//! Logical matches (spec 13, 16.5, 17): creating them, reading them,
//! cancelling them and organizer adjudication. Fighters' claims, permits and
//! reports are in `play`. Each decided game is an attempt row with exactly one
//! score effect, so retries and duplicate deliveries cannot add a win (SEC-09).
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
    matches::{CreateMatch, MatchState, Participant as Assigned, Resolution, Rules},
    play::play_url,
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::collections::BTreeMap;

use crate::{
    AppState,
    auth::{self, Actor, Role, Service},
    config::Policy,
    error::{ApiFailure, Result},
    events,
    http::{Body, GENERAL_BODY, expected_revision, idempotency_key, ok},
    routes::{
        ledger::{Cause, Match, bump, load, match_event, release, settle, stale},
        links::{Ctx, audit},
        lobbies, tournaments,
    },
};

pub use crate::routes::ledger::{busy, participants, release_players, scores};

/// Results entered by an organizer; Ember plays no part in the match.
pub const ORGANIZER_PROFILE: &str = "organizer-reported-v1";
/// The rules profiles a match may use. `ember-room-v1` games are played in an
/// Ember room under the room's own settings and reported by both fighters.
/// Native profiles such as `usf4-standard-v1` are refused until a tested
/// rules translator exists.
pub const PROFILES: [&str; 2] = [ORGANIZER_PROFILE, ember_protocol::play::PROFILE];
const MAX_REASON: usize = 512;
const MAX_EVIDENCE: usize = 16;

/// Who may see a match: its assigned players, its provider connection, or
/// an organizer of its tenant. Anyone else gets `not_found`.
pub fn visible(tx: &Transaction<'_>, actor: &Viewer, found: &Match) -> Result<bool> {
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
    if command.external_match_id.starts_with(lobbies::MATCH_PREFIX)
        || command
            .external_match_id
            .starts_with(tournaments::MATCH_PREFIX)
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
            lobby_id: None,
            tournament_id: None,
        },
    )?;
    let found = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
    Ok((StatusCode::CREATED, created_body(ctx, &found)))
}

/// A match about to be created, by a provider or by a lobby for its next set.
pub struct NewMatch<'a> {
    pub tenant_id: &'a str,
    pub connection_id: &'a str,
    pub external_match_id: &'a str,
    pub digest: &'a str,
    pub rules: &'a Rules,
    pub required_build_id: &'a str,
    pub metadata: &'a BTreeMap<String, String>,
    /// Ordered by slot.
    pub participants: [&'a Assigned; 2],
    pub lobby_id: Option<&'a str>,
    pub tournament_id: Option<&'a str>,
}

/// Whether `participant_id` on the connection is currently linked to `ember_id`.
pub fn linked(
    tx: &Transaction<'_>,
    connection_id: &str,
    participant_id: &str,
    ember_id: &EmberId,
) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM external_accounts a JOIN links l ON l.account_id = a.id
          WHERE a.connection_id = ?1 AND a.participant_id = ?2 AND l.ember_id = ?3 AND l.revoked_at IS NULL)",
        params![connection_id, participant_id, ember_id.as_str()],
        |row| row.get(0),
    )?)
}

/// Checks the roster against current links and other active matches, then
/// records the match and its `match.created` event. Returns the new ID.
pub fn insert_match(tx: &Transaction<'_>, ctx: &Ctx, new: &NewMatch<'_>) -> Result<String> {
    // Every creation path meets here, so the two fighters are told apart here.
    let [first, second] = new.participants;
    if first.ember_id == second.ember_id || first.participant_id == second.participant_id {
        return Err(ApiFailure::invalid(
            "A match is between two different players.",
        ));
    }
    for participant in new.participants {
        if !linked(
            tx,
            new.connection_id,
            &participant.participant_id,
            &participant.ember_id,
        )? {
            return Err(ApiFailure::invalid(
                "A participant is not currently linked to that Ember ID on this connection.",
            )
            .detail("slot", participant.slot));
        }
    }
    for participant in new.participants {
        if busy(tx, new.connection_id, &participant.ember_id, "")? {
            return Err(ApiFailure::new(
                ErrorCode::LeaseConflict,
                "A participant already has an active match.",
            )
            .detail("slot", participant.slot));
        }
    }
    let id = crate::util::new_id("emt");
    let delivery = if policy(tx, new.connection_id)?.sends_results {
        "queued"
    } else {
        "not_required"
    };
    tx.execute(
        "INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
            assignment_generation, state, games_to_win, rules, required_build_id, metadata, delivery_state,
            created_at, updated_at, lobby_id, tournament_id)
         VALUES (?1, ?2, ?3, ?4, ?5, 1, 1, 'awaiting_players', ?6, ?7, ?8, ?9, ?13, ?10, ?10, ?11, ?12)",
        params![
            id,
            new.tenant_id,
            new.connection_id,
            new.external_match_id,
            new.digest,
            new.rules.games_to_win,
            serde_json::to_string(new.rules).unwrap_or_default(),
            new.required_build_id,
            serde_json::to_string(new.metadata).unwrap_or_default(),
            ctx.now,
            new.lobby_id,
            new.tournament_id,
            delivery
        ],
    )?;
    for participant in new.participants {
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
            "external_match_id": new.external_match_id,
            "match_revision": "1",
            "assignment_generation": "1",
            "state": MatchState::AwaitingPlayers,
            "games_to_win": new.rules.games_to_win,
            "participants": roster,
            "metadata": new.metadata,
        }),
    )?;
    Ok(id)
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

/// A consistent snapshot plus the event cursor from the same read (17.3).
fn snapshot(tx: &Transaction<'_>, bridge_id: &str, found: &Match) -> Result<serde_json::Value> {
    let roster = participants(tx, &found.id, found.generation)?;
    let (wins, _) = scores(tx, &found.id)?;
    let (rules, metadata): (String, String) = tx.query_row(
        "SELECT rules, metadata FROM matches WHERE id = ?1",
        [&found.id],
        |row| Ok((row.get(0)?, row.get(1)?)),
    )?;
    let attempts = tx
        .prepare(
            "SELECT id, seq, outcome, state, source, adjudication_id FROM attempts WHERE match_id = ?1 ORDER BY seq",
        )?
        .query_map([&found.id], |row| {
            Ok(json!({
                "attempt_id": row.get::<_, String>(0)?,
                "seq": row.get::<_, u64>(1)?,
                "outcome": row.get::<_, Option<String>>(2)?,
                "state": row.get::<_, String>(3)?,
                "source": row.get::<_, String>(4)?,
                "adjudication_id": row.get::<_, Option<String>>(5)?,
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
        "play_url": play_url(bridge_id, &found.id),
        "assignment_generation": found.generation.to_string(),
        "games_to_win": found.games_to_win,
        "rules": serde_json::from_str::<serde_json::Value>(&rules).unwrap_or_default(),
        "participants": roster,
        "scores": scores,
        "attempts": attempts,
        "provider_delivery_state": found.delivery,
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
    let bridge_id = state.config.bridge_id.clone();
    let body = state
        .db
        .read(move |tx| {
            let found = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &found)? {
                return Err(ApiFailure::not_found());
            }
            snapshot(tx, &bridge_id, &found)
        })
        .await?;
    Ok(ok(&body))
}

/// `GET /v1/assignments`: the caller's own matches only, with what a player
/// needs to pick one: the set length and score, the round, the opponent's
/// fingerprint and whether the game is played through Ember.
pub async fn assignments(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let config = state.config.clone();
    let body = state
        .db
        .read(move |tx| {
            let rows = tx
                .prepare(
                    "SELECT m.id, m.connection_id, m.state, m.revision, p.slot, m.games_to_win, m.rules, m.metadata,
                            o.ember_id
                     FROM matches m
                     JOIN match_participants p ON p.match_id = m.id AND p.assignment_generation = m.assignment_generation
                     JOIN match_participants o ON o.match_id = m.id AND o.assignment_generation = m.assignment_generation
                       AND o.slot != p.slot
                     WHERE p.ember_id = ?1 ORDER BY (m.state IN ('completed', 'cancelled', 'failed')), m.updated_at DESC
                     LIMIT 50",
                )?
                .query_map([player.ember_id.as_str()], |row| {
                    Ok((
                        row.get::<_, String>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, String>(2)?,
                        row.get::<_, u64>(3)?,
                        row.get::<_, u8>(4)?,
                        row.get::<_, u8>(5)?,
                        row.get::<_, String>(6)?,
                        row.get::<_, String>(7)?,
                        row.get::<_, String>(8)?,
                    ))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            let mut assignments = Vec::with_capacity(rows.len());
            for (id, connection, state, revision, slot, games_to_win, rules, metadata, opponent) in rows {
                let (wins, _) = scores(tx, &id)?;
                let rules: serde_json::Value = serde_json::from_str(&rules).unwrap_or_default();
                let metadata: serde_json::Value = serde_json::from_str(&metadata).unwrap_or_default();
                let opponent = EmberId::parse(&opponent).map_err(|_| ApiFailure::unavailable())?;
                assignments.push(json!({
                    "match_id": id,
                    "provider": config.connection(&connection).map(|(_, c)| c.display_name.clone()),
                    "state": state,
                    "revision": revision.to_string(),
                    "slot": slot,
                    "games_to_win": games_to_win,
                    "native_rules_profile": rules.get("native_rules_profile"),
                    "round_label": metadata.get("round_label"),
                    "opponent": { "ember_id": opponent, "fingerprint": opponent.fingerprint() },
                    "wins": wins,
                }));
            }
            Ok(json!({ "assignments": assignments, "event_cursor": events::head(tx)?.to_string() }))
        })
        .await?;
    Ok(ok(&body))
}

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
                let revision = cancel_and_release(tx, &ctx, &found, &command.reason)?;
                audit(tx, ctx.now, ("service", service.id.clone()), "match.cancel", &found.id, "ok", Some(&command.reason))?;
                Ok((StatusCode::OK, json!({ "match_id": found.id, "state": "cancelled", "revision": revision.to_string() })))
            })
        })
        .await?;
    state.committed();
    Ok(respond(result))
}

/// Cancels a match and returns its players to their other matches, as the
/// cancel route does. Returns the new revision.
pub fn cancel_and_release(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    found: &Match,
    reason: &str,
) -> Result<u64> {
    let revision = cancel_match(tx, ctx, found, reason)?;
    release(
        tx,
        ctx,
        found,
        &participants(tx, &found.id, found.generation)?,
    )?;
    Ok(revision)
}

fn cancel_match(tx: &Transaction<'_>, ctx: &Ctx, found: &Match, reason: &str) -> Result<u64> {
    // A game the fighters were playing counts for nobody; reports that come
    // later are kept as evidence only.
    tx.execute(
        "UPDATE attempts SET state = 'aborted' WHERE match_id = ?1 AND state IN ('permitted', 'review')",
        [&found.id],
    )?;
    let revision = bump(tx, found, MatchState::Cancelled, ctx.now)?;
    match_event(
        tx,
        ctx,
        found,
        Kind::MatchCancelled,
        json!({ "match_id": found.id, "match_revision": revision.to_string(), "state": "cancelled", "reason": reason }),
    )?;
    Ok(revision)
}

/// Cancels a lobby's running set when a seated player leaves or the lobby
/// closes. A finished set is left as it is. A tournament set waiting on its
/// players can start; the lobby itself moves on in its own code.
pub fn cancel_lobby_set(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    match_id: &str,
    reason: &str,
) -> Result<()> {
    let found = load(tx, match_id)?.ok_or_else(ApiFailure::unavailable)?;
    if !found.state.is_terminal() {
        cancel_match(tx, ctx, &found, reason)?;
        let players: Vec<EmberId> = participants(tx, &found.id, found.generation)?
            .into_iter()
            .map(|p| p.ember_id)
            .collect();
        tournaments::on_players_free(tx, ctx, &found.connection_id, &players)?;
    }
    Ok(())
}

/// Cancels a tournament set that a withdrawal, a correction or the
/// tournament's end has made moot, returning its players.
pub fn cancel_tournament_set(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    match_id: &str,
    reason: &str,
) -> Result<Vec<EmberId>> {
    let found = load(tx, match_id)?.ok_or_else(ApiFailure::unavailable)?;
    if found.state.is_terminal() {
        return Ok(Vec::new());
    }
    cancel_match(tx, ctx, &found, reason)?;
    Ok(participants(tx, &found.id, found.generation)?
        .into_iter()
        .map(|p| p.ember_id)
        .collect())
}

/// Whether a match has an accepted game.
pub fn has_games(tx: &Transaction<'_>, match_id: &str) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM attempts WHERE match_id = ?1 AND state = 'accepted')",
        [match_id],
        |row| row.get(0),
    )?)
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
    /// A canonical decimal string, like every revision (spec 9.2).
    #[serde(default)]
    expected_revision: Option<Counter>,
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
    let expected = expected_revision(
        &headers,
        command.expected_revision.map(|revision| revision.0),
    )?;
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
            let open: Option<(String, u64)> = tx
                .query_row(
                    "SELECT id, seq FROM attempts WHERE match_id = ?1 AND state IN ('permitted', 'review')",
                    [&found.id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?;
            record_adjudication(tx, ctx, service, found, &adjudication_id, command, expected)?;
            // A game the fighters played and could not settle is decided in
            // place; otherwise the result is a new organizer attempt.
            let (attempt_id, seq) = match (&command.attempt_id, open) {
                (Some(named), Some((open_id, seq))) if *named == open_id => {
                    tx.execute(
                        "UPDATE attempts SET state = 'accepted', outcome = ?1, adjudication_id = ?2 WHERE id = ?3",
                        params![outcome, adjudication_id, open_id],
                    )?;
                    (open_id, seq)
                }
                (Some(_), _) => {
                    return Err(ApiFailure::invalid(
                        "That attempt is not this match's open game.",
                    ));
                }
                (None, Some((open_id, _))) => {
                    return Err(ApiFailure::invalid(
                        "This match has a played game waiting for a result. Decide it by its attempt_id.",
                    )
                    .detail("attempt_id", open_id));
                }
                (None, None) => {
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
                    (attempt_id, seq)
                }
            };
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
            // An accepted game, or a played game still waiting for a result.
            let changed = tx.execute(
                "UPDATE attempts SET state = 'voided', voided_by = ?1
                 WHERE id = ?2 AND match_id = ?3 AND state IN ('accepted', 'permitted', 'review')",
                params![adjudication_id, attempt, found.id],
            )?;
            if changed != 1 {
                return Err(ApiFailure::invalid(
                    "That attempt is not a game of this match that can be voided.",
                ));
            }
        }
    }
    let settled = settle(
        tx,
        ctx,
        found,
        &roster,
        wins_before,
        Cause::Organizer {
            adjudication_id: &adjudication_id,
            reason: &command.reason,
        },
        events,
    )?;
    Ok((
        StatusCode::CREATED,
        json!({
            "adjudication_id": adjudication_id,
            "match_id": found.id,
            "state": settled.state,
            "revision": settled.revision.to_string(),
            "scores": settled.scores,
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
/// match on that connection that names it goes to review (spec 11.6). Its
/// lobby places end instead, which cancels a lobby set it was playing.
pub fn on_unlink(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    _tenant: &str,
    connection_id: &str,
    ember_id: &EmberId,
) -> Result<()> {
    lobbies::on_unlink(tx, ctx, connection_id, ember_id)?;
    tournaments::on_unlink(tx, ctx, connection_id, ember_id)?;
    let ids = tx
        .prepare(
            "SELECT m.id FROM matches m JOIN match_participants p
               ON p.match_id = m.id AND p.assignment_generation = m.assignment_generation
             WHERE m.connection_id = ?1 AND p.ember_id = ?2 AND m.lobby_id IS NULL AND m.tournament_id IS NULL
               AND m.state NOT IN ('completed', 'cancelled', 'failed', 'needs_review')",
        )?
        .query_map(params![connection_id, ember_id.as_str()], |row| {
            row.get::<_, String>(0)
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for id in ids {
        let found = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
        // A game in progress waits for the organizer with the rest of the match.
        tx.execute(
            "UPDATE attempts SET state = 'review' WHERE match_id = ?1 AND state = 'permitted'",
            [&id],
        )?;
        needs_review(tx, ctx, &found, "identity_unlinked", None)?;
    }
    Ok(())
}

/// The policy of a connection's platform, from the kind its record keeps.
/// A kind never changes, and the record stays when the connection leaves the
/// configuration, so its matches keep their policy.
fn policy(tx: &Transaction<'_>, connection_id: &str) -> Result<Policy> {
    let kind: String = tx
        .query_row(
            "SELECT kind FROM provider_connections WHERE id = ?1",
            [connection_id],
            |row| row.get(0),
        )
        .optional()?
        .ok_or_else(ApiFailure::unavailable)?;
    Ok(Policy::of(&kind))
}

/// A match whose result needs a person (spec 16.6) waits in `needs_review`
/// for its organizer. Where the platform has no review
/// (`Policy::reviews_disputes`), it is cancelled instead and its players are
/// free. `attempt_id` is the game held, when one is.
pub fn needs_review(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    found: &Match,
    reason: &str,
    attempt_id: Option<&str>,
) -> Result<()> {
    if !policy(tx, &found.connection_id)?.reviews_disputes {
        cancel_and_release(tx, ctx, found, reason)?;
        return Ok(());
    }
    let revision = bump(tx, found, MatchState::NeedsReview, ctx.now)?;
    let mut data = json!({
        "match_id": found.id,
        "match_revision": revision.to_string(),
        "state": MatchState::NeedsReview,
        "reason": reason,
    });
    if let Some(attempt_id) = attempt_id {
        data["attempt_id"] = json!(attempt_id);
    }
    match_event(tx, ctx, found, Kind::NeedsReview, data)?;
    Ok(())
}

/// Any response type, for routes that share the idempotency helper.
pub fn into_response(result: (StatusCode, serde_json::Value)) -> Response {
    respond(result).into_response()
}
