//! Lobbies: an opt-in queue on a provider connection that plays one
//! first-to-N set after another (an extension beyond EMBER-TB-001).
//!
//! Each set is an ordinary match, so scoring, adjudication and the busy check
//! are the match code's. When a set completes the lobby rotates its seats in
//! the same transaction: the players the rotation sends away join the back of
//! the queue and the next set starts with whoever is first. Only players the
//! provider queued at their own request are ever in the queue.
use std::collections::BTreeMap;

use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    encoding::{Counter, is_prefixed_id},
    event::Kind,
    json,
    lobby::{CreateLobby, JoinLobby, Rotation},
    matches::{Participant as Assigned, Rules, Score},
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::{
    AppState,
    auth::{self, Role},
    error::{ApiFailure, Result},
    events::{self, NewEvent, Viewer, emit},
    http::{Body, GENERAL_BODY, expected_revision, idempotency_key, ok},
    routes::{
        links::{Ctx, audit},
        matches::{
            self, NewMatch, ORGANIZER_PROFILE, idempotent, into_response, service_viewer, viewer_of,
        },
        records,
    },
};

/// `external_match_id` prefix of lobby sets; providers cannot create one.
pub const MATCH_PREFIX: &str = "lobby:";
/// Players queued or seated in one lobby at most.
pub const MAX_PLAYERS: i64 = 64;

struct Lobby {
    id: String,
    tenant_id: String,
    connection_id: String,
    external_lobby_id: String,
    revision: u64,
    open: bool,
    games_to_win: u8,
    rotation: Rotation,
    required_build_id: String,
    metadata: BTreeMap<String, String>,
    current_match_id: Option<String>,
    sets_started: u64,
    sets_completed: u64,
    streak_holder: Option<String>,
    streak: u64,
}

fn load(tx: &Transaction<'_>, id: &str) -> Result<Option<Lobby>> {
    tx.query_row(
        "SELECT id, tenant_id, connection_id, external_lobby_id, revision, state, games_to_win, rotation,
                required_build_id, metadata, current_match_id, sets_started, sets_completed, streak_holder, streak
         FROM lobbies WHERE id = ?1",
        [id],
        |row| {
            Ok(Lobby {
                id: row.get(0)?,
                tenant_id: row.get(1)?,
                connection_id: row.get(2)?,
                external_lobby_id: row.get(3)?,
                revision: row.get(4)?,
                open: row.get::<_, String>(5)? == "open",
                games_to_win: row.get(6)?,
                rotation: Rotation::parse(&row.get::<_, String>(7)?).unwrap_or(Rotation::WinnerStays),
                required_build_id: row.get(8)?,
                metadata: serde_json::from_str(&row.get::<_, String>(9)?).unwrap_or_default(),
                current_match_id: row.get(10)?,
                sets_started: row.get(11)?,
                sets_completed: row.get(12)?,
                streak_holder: row.get(13)?,
                streak: row.get(14)?,
            })
        },
    )
    .optional()
    .map_err(Into::into)
}

#[derive(Clone, Debug, Serialize)]
struct Entry {
    participant_id: String,
    ember_id: EmberId,
    #[serde(skip_serializing_if = "Option::is_none")]
    slot: Option<u8>,
}

fn entries(tx: &Transaction<'_>, lobby_id: &str, state: &str) -> Result<Vec<Entry>> {
    tx.prepare(
        "SELECT participant_id, ember_id, slot FROM lobby_entries WHERE lobby_id = ?1 AND state = ?2
         ORDER BY position, slot",
    )?
    .query_map(params![lobby_id, state], |row| {
        Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?, row.get::<_, Option<u8>>(2)?))
    })?
    .map(|row| {
        let (participant_id, ember_id, slot) = row?;
        Ok(Entry {
            participant_id,
            ember_id: EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?,
            slot,
        })
    })
    .collect()
}

fn queue(tx: &Transaction<'_>, lobby_id: &str) -> Result<Vec<Entry>> {
    entries(tx, lobby_id, "queued")
}

/// Seated players by slot.
fn seats(tx: &Transaction<'_>, lobby_id: &str) -> Result<[Option<Entry>; 2]> {
    let mut seats = [None, None];
    for entry in entries(tx, lobby_id, "seated")? {
        if let Some(slot) = entry.slot.filter(|slot| *slot < 2) {
            seats[usize::from(slot)] = Some(entry);
        }
    }
    Ok(seats)
}

fn seated_ids(tx: &Transaction<'_>, lobby_id: &str) -> Result<[Option<EmberId>; 2]> {
    Ok(seats(tx, lobby_id)?.map(|entry| entry.map(|entry| entry.ember_id)))
}

fn ids(entries: &[Entry]) -> Vec<&EmberId> {
    entries.iter().map(|entry| &entry.ember_id).collect()
}

fn visible(tx: &Transaction<'_>, viewer: &Viewer, lobby: &Lobby) -> Result<bool> {
    Ok(match viewer {
        Viewer::Provider { connection_id, .. } => *connection_id == lobby.connection_id,
        Viewer::Organizer { tenant_id } => *tenant_id == lobby.tenant_id,
        Viewer::Player { ember_id } => tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM lobby_entries WHERE lobby_id = ?1 AND ember_id = ?2)",
            params![lobby.id, ember_id.as_str()],
            |row| row.get(0),
        )?,
    })
}

fn touch(tx: &Transaction<'_>, lobby_id: &str, now: u64) -> Result<u64> {
    Ok(tx.query_row(
        "UPDATE lobbies SET revision = revision + 1, updated_at = ?2 WHERE id = ?1 RETURNING revision",
        params![lobby_id, now],
        |row| row.get(0),
    )?)
}

fn lobby_event(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    lobby: &Lobby,
    kind: Kind,
    data: serde_json::Value,
) -> Result<i64> {
    emit(
        tx,
        &ctx.config,
        ctx.now,
        NewEvent {
            kind,
            tenant_id: &lobby.tenant_id,
            connection_id: Some(&lobby.connection_id),
            subject: format!("lobbies/{}", lobby.id),
            match_id: None,
            ember_id: None,
            lobby_id: Some(&lobby.id),
            data,
        },
    )
}

fn set_entry(
    tx: &Transaction<'_>,
    lobby_id: &str,
    ember_id: &EmberId,
    state: &str,
    slot: Option<u8>,
    now: u64,
) -> Result<()> {
    // Joining the queue (again) takes the next place at the back.
    let position: Option<i64> = if state == "queued" {
        Some(tx.query_row(
            "UPDATE lobbies SET next_position = next_position + 1 WHERE id = ?1 RETURNING next_position - 1",
            [lobby_id],
            |row| row.get(0),
        )?)
    } else {
        None
    };
    tx.execute(
        "UPDATE lobby_entries SET state = ?3, slot = ?4, position = ?5, updated_at = ?6
         WHERE lobby_id = ?1 AND ember_id = ?2",
        params![lobby_id, ember_id.as_str(), state, slot, position, now],
    )?;
    Ok(())
}

/// The next set when both seats can be filled and none is running. Empty
/// seats take the first queued players who are still linked and have no other
/// active match on the connection; a player busy elsewhere keeps their place.
/// A player whose link ended leaves the queue. Returns the started set, which
/// the caller announces after its own event.
fn advance(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    lobby_id: &str,
    dropped: &mut Vec<EmberId>,
) -> Result<Option<serde_json::Value>> {
    let lobby = load(tx, lobby_id)?.ok_or_else(ApiFailure::unavailable)?;
    if !lobby.open || lobby.current_match_id.is_some() {
        return Ok(None);
    }
    let mut seated = seats(tx, &lobby.id)?;
    for slot in 0..2u8 {
        if seated[usize::from(slot)].is_some() {
            continue;
        }
        for candidate in queue(tx, &lobby.id)? {
            if !matches::linked(
                tx,
                &lobby.connection_id,
                &candidate.participant_id,
                &candidate.ember_id,
            )? {
                set_entry(tx, &lobby.id, &candidate.ember_id, "left", None, ctx.now)?;
                dropped.push(candidate.ember_id.clone());
                continue;
            }
            if matches::busy(tx, &lobby.connection_id, &candidate.ember_id, "")? {
                continue;
            }
            set_entry(
                tx,
                &lobby.id,
                &candidate.ember_id,
                "seated",
                Some(slot),
                ctx.now,
            )?;
            seated[usize::from(slot)] = Some(Entry {
                slot: Some(slot),
                ..candidate
            });
            break;
        }
    }
    let [Some(first), Some(second)] = &seated else {
        return Ok(None);
    };
    // A seated player who was given another match while waiting for an
    // opponent keeps the seat; the set starts when that match ends.
    for entry in [first, second] {
        if matches::busy(tx, &lobby.connection_id, &entry.ember_id, "")? {
            return Ok(None);
        }
    }
    let number = lobby.sets_started + 1;
    let roster = [first, second].map(|entry| Assigned {
        participant_id: entry.participant_id.clone(),
        ember_id: entry.ember_id.clone(),
        slot: entry.slot.unwrap_or(0),
    });
    let rules = Rules {
        games_to_win: lobby.games_to_win,
        draw_policy: "replay_no_score".into(),
        native_rules_profile: ORGANIZER_PROFILE.into(),
        edition_policy: "ultra_only".into(),
        character_policy: "unrestricted_between_games".into(),
        stage_policy: "p1_selects".into(),
        input_delay_policy: "ember_existing_ready_policy".into(),
    };
    let external_match_id = format!("{MATCH_PREFIX}{}:{number}", lobby.id);
    let digest = json::digest(&json!({ "lobby_id": lobby.id, "set": number }))
        .map_err(|_| ApiFailure::unavailable())?;
    let mut metadata = lobby.metadata.clone();
    if metadata.len() < ember_protocol::matches::MAX_METADATA_ENTRIES {
        metadata.insert("lobby_set".into(), number.to_string());
    }
    let match_id = matches::insert_match(
        tx,
        ctx,
        &NewMatch {
            tenant_id: &lobby.tenant_id,
            connection_id: &lobby.connection_id,
            external_match_id: &external_match_id,
            digest: &digest,
            rules: &rules,
            required_build_id: &lobby.required_build_id,
            metadata: &metadata,
            participants: [&roster[0], &roster[1]],
            lobby_id: Some(&lobby.id),
        },
    )?;
    tx.execute(
        "UPDATE lobbies SET current_match_id = ?2, sets_started = ?3 WHERE id = ?1",
        params![lobby.id, match_id, number],
    )?;
    Ok(Some(json!({
        "lobby_id": lobby.id,
        "match_id": match_id,
        "set": number,
        "games_to_win": lobby.games_to_win,
        "participants": roster,
    })))
}

/// Ends a streak whose holder no longer sits at the lobby.
fn keep_streak(tx: &Transaction<'_>, lobby_id: &str) -> Result<()> {
    tx.execute(
        "UPDATE lobbies SET streak_holder = NULL, streak = 0 WHERE id = ?1 AND streak_holder IS NOT NULL
           AND streak_holder NOT IN (SELECT ember_id FROM lobby_entries WHERE lobby_id = ?1 AND state = 'seated')",
        [lobby_id],
    )?;
    Ok(())
}

/// Emits the queue's new order, then the set that change started, if any.
fn announce(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    lobby_id: &str,
    reason: &str,
    ember_id: Option<&EmberId>,
    dropped: &[EmberId],
    started: Option<serde_json::Value>,
) -> Result<u64> {
    keep_streak(tx, lobby_id)?;
    let revision = touch(tx, lobby_id, ctx.now)?;
    let lobby = load(tx, lobby_id)?.ok_or_else(ApiFailure::unavailable)?;
    let waiting = queue(tx, lobby_id)?;
    let seated = seats(tx, lobby_id)?;
    lobby_event(
        tx,
        ctx,
        &lobby,
        Kind::LobbyQueueChanged,
        json!({
            "lobby_id": lobby.id,
            "lobby_revision": revision.to_string(),
            "reason": reason,
            "ember_id": ember_id,
            "removed": dropped,
            "seated": seated.iter().flatten().collect::<Vec<_>>(),
            "queue": ids(&waiting),
        }),
    )?;
    if let Some(mut data) = started {
        data["lobby_revision"] = revision.to_string().into();
        data["queue"] = json!(ids(&waiting));
        lobby_event(tx, ctx, &lobby, Kind::LobbySetStarted, data)?;
    }
    Ok(revision)
}

/// Called by the match code in the transaction that completes a lobby set.
pub fn on_set_completed(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    lobby_id: &str,
    match_id: &str,
    winner_slot: u8,
    scores: &[Score],
) -> Result<()> {
    let lobby = load(tx, lobby_id)?.ok_or_else(ApiFailure::unavailable)?;
    if lobby.current_match_id.as_deref() != Some(match_id) {
        return Ok(());
    }
    let seated = seats(tx, &lobby.id)?;
    let (Some(winner), Some(loser)) = (
        seated[usize::from(winner_slot)].clone(),
        seated[usize::from(1 - winner_slot)].clone(),
    ) else {
        return Err(ApiFailure::unavailable());
    };
    let streak = if lobby.streak_holder.as_deref() == Some(winner.ember_id.as_str()) {
        lobby.streak.saturating_add(1)
    } else {
        1
    };
    tx.execute(
        "UPDATE lobbies SET current_match_id = NULL, sets_completed = sets_completed + 1, streak_holder = ?2, streak = ?3
         WHERE id = ?1",
        params![lobby.id, winner.ember_id.as_str(), streak],
    )?;
    tx.execute(
        "UPDATE lobby_entries SET best_streak = MAX(best_streak, ?3) WHERE lobby_id = ?1 AND ember_id = ?2",
        params![lobby.id, winner.ember_id.as_str(), streak],
    )?;
    for slot in lobby.rotation.leaving(winner_slot) {
        if let Some(leaving) = &seated[usize::from(slot)] {
            set_entry(tx, &lobby.id, &leaving.ember_id, "queued", None, ctx.now)?;
        }
    }
    let mut dropped = Vec::new();
    let started = advance(tx, ctx, &lobby.id, &mut dropped)?;
    keep_streak(tx, &lobby.id)?;
    let revision = touch(tx, &lobby.id, ctx.now)?;
    let after = load(tx, &lobby.id)?.ok_or_else(ApiFailure::unavailable)?;
    let waiting = queue(tx, &lobby.id)?;
    lobby_event(
        tx,
        ctx,
        &after,
        Kind::LobbySetCompleted,
        json!({
            "lobby_id": after.id,
            "lobby_revision": revision.to_string(),
            "match_id": match_id,
            "set": lobby.sets_started,
            "games_to_win": lobby.games_to_win,
            "rotation": lobby.rotation,
            "winner_id": winner.ember_id,
            "loser_id": loser.ember_id,
            "scores": scores,
            "streak": after.streak_holder.as_ref().map(|holder| json!({ "ember_id": holder, "sets": after.streak })),
            "removed": dropped,
            "queue": ids(&waiting),
        }),
    )?;
    if let Some(mut data) = started {
        data["lobby_revision"] = revision.to_string().into();
        data["queue"] = json!(ids(&waiting));
        lobby_event(tx, ctx, &after, Kind::LobbySetStarted, data)?;
    }
    Ok(())
}

/// Takes a player out of a lobby. A seated player's running set is
/// cancelled; the other player keeps the seat and the next queued player sits.
fn remove(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    lobby: &Lobby,
    ember_id: &EmberId,
    reason: &str,
) -> Result<u64> {
    let state: Option<String> = tx
        .query_row(
            "SELECT state FROM lobby_entries WHERE lobby_id = ?1 AND ember_id = ?2",
            params![lobby.id, ember_id.as_str()],
            |row| row.get(0),
        )
        .optional()?;
    if state.as_deref() == Some("seated")
        && let Some(current) = &lobby.current_match_id
    {
        matches::cancel_lobby_set(tx, ctx, current, reason)?;
        tx.execute(
            "UPDATE lobbies SET current_match_id = NULL WHERE id = ?1",
            [&lobby.id],
        )?;
    }
    set_entry(tx, &lobby.id, ember_id, "left", None, ctx.now)?;
    let mut dropped = Vec::new();
    let started = advance(tx, ctx, &lobby.id, &mut dropped)?;
    announce(
        tx,
        ctx,
        &lobby.id,
        reason,
        Some(ember_id),
        &dropped,
        started,
    )
}

/// Called when a match outside any lobby completes or is cancelled. Its
/// players are free again, which may be all an idle lobby was waiting for.
pub fn on_players_free(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    players: &[EmberId],
) -> Result<()> {
    for ember_id in players {
        let lobbies = tx
            .prepare(
                "SELECT l.id FROM lobbies l JOIN lobby_entries e ON e.lobby_id = l.id
                 WHERE l.connection_id = ?1 AND l.state = 'open' AND l.current_match_id IS NULL
                   AND e.ember_id = ?2 AND e.state != 'left'",
            )?
            .query_map(params![connection_id, ember_id.as_str()], |row| {
                row.get::<_, String>(0)
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        for id in lobbies {
            // A freed player can take a seat even when the set cannot start
            // yet; that is a change the lobby announces like any other.
            let before = seated_ids(tx, &id)?;
            let mut dropped = Vec::new();
            let started = advance(tx, ctx, &id, &mut dropped)?;
            if started.is_some() || !dropped.is_empty() || seated_ids(tx, &id)? != before {
                announce(
                    tx,
                    ctx,
                    &id,
                    "player_available",
                    Some(ember_id),
                    &dropped,
                    started,
                )?;
            }
        }
    }
    Ok(())
}

/// An ended link ends that player's places in the connection's open lobbies.
pub fn on_unlink(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    ember_id: &EmberId,
) -> Result<()> {
    let lobbies = tx
        .prepare(
            "SELECT l.id FROM lobbies l JOIN lobby_entries e ON e.lobby_id = l.id
             WHERE l.connection_id = ?1 AND l.state = 'open' AND e.ember_id = ?2 AND e.state != 'left'",
        )?
        .query_map(params![connection_id, ember_id.as_str()], |row| row.get::<_, String>(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for id in lobbies {
        let lobby = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
        remove(tx, ctx, &lobby, ember_id, "identity_unlinked")?;
    }
    Ok(())
}

fn snapshot(tx: &Transaction<'_>, lobby: &Lobby) -> Result<serde_json::Value> {
    Ok(json!({
        "lobby_id": lobby.id,
        "external_lobby_id": lobby.external_lobby_id,
        "state": if lobby.open { "open" } else { "closed" },
        "revision": lobby.revision.to_string(),
        "games_to_win": lobby.games_to_win,
        "rotation": lobby.rotation,
        "required_build_id": lobby.required_build_id,
        "current_match_id": lobby.current_match_id,
        "sets_started": lobby.sets_started,
        "sets_completed": lobby.sets_completed,
        "seated": seats(tx, &lobby.id)?.iter().flatten().collect::<Vec<_>>(),
        "queue": queue(tx, &lobby.id)?,
        "streak": lobby.streak_holder.as_ref().map(|holder| json!({ "ember_id": holder, "sets": lobby.streak })),
        "standings": records::lobby_standings(tx, &lobby.id)?,
        "metadata": lobby.metadata,
        "event_cursor": events::head(tx)?.to_string(),
    }))
}

fn closed() -> ApiFailure {
    ApiFailure::new(ErrorCode::StaleRevision, "The lobby is closed.")
}

/// `POST /v1/lobbies`: a provider opens a lobby on its connection.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let key = idempotency_key(&headers)?;
    let command: CreateLobby = body.parse()?;
    command.check()?;
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let tenant_id = service.tenant_id.clone();
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, "/v1/lobbies", &key, &digest, ctx.now, |tx| {
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
                    &ctx,
                    &lobby,
                    Kind::LobbyCreated,
                    json!({
                        "lobby_id": id,
                        "external_lobby_id": command.external_lobby_id,
                        "lobby_revision": "1",
                        "games_to_win": command.games_to_win,
                        "rotation": command.rotation,
                        "metadata": command.metadata,
                    }),
                )?;
                Ok((StatusCode::CREATED, snapshot(tx, &lobby)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}

/// `GET /v1/lobbies/{id}`: its provider, an organizer of its tenant, or a
/// player who has joined it.
pub async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let viewer = viewer_of(&auth::any(&state, &headers).await?)?;
    let body = state
        .db
        .read(move |tx| {
            let lobby = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &lobby)? {
                return Err(ApiFailure::not_found());
            }
            snapshot(tx, &lobby)
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
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/lobbies/{id}/queue");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let lobby = load(tx, &id)?.filter(|lobby| lobby.connection_id == connection_id).ok_or_else(ApiFailure::not_found)?;
                if !lobby.open {
                    return Err(closed());
                }
                if !matches::linked(tx, &connection_id, &command.participant_id, &command.ember_id)? {
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
                let started = advance(tx, &ctx, &lobby.id, &mut dropped)?;
                announce(tx, &ctx, &lobby.id, "joined", Some(&command.ember_id), &dropped, started)?;
                let lobby = load(tx, &lobby.id)?.ok_or_else(ApiFailure::unavailable)?;
                Ok((StatusCode::CREATED, snapshot(tx, &lobby)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
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
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/lobbies/{id}/queue/{participant}/leave");
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let lobby = load(tx, &id)?.filter(|lobby| lobby.connection_id == connection_id).ok_or_else(ApiFailure::not_found)?;
                if expected.is_some_and(|expected| expected != lobby.revision) {
                    return Err(ApiFailure::new(ErrorCode::StaleRevision, "The lobby changed. Refresh it and try again.")
                        .detail("current_revision", lobby.revision.to_string()));
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
                    remove(tx, &ctx, &lobby, &ember_id, "left")?;
                    audit(tx, ctx.now, ("service", service.id.clone()), "lobby.leave", &lobby.id, "ok", None)?;
                }
                let lobby = load(tx, &lobby.id)?.ok_or_else(ApiFailure::unavailable)?;
                Ok((StatusCode::OK, snapshot(tx, &lobby)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
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
    let viewer = service_viewer(&service)?;
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
    let digest = json::digest(&command)?;
    let ctx = Ctx::of(&state);
    let path = format!("/v1/lobbies/{id}/close");
    let actor = match service.role {
        Role::Provider => "service",
        Role::Organizer => "organizer",
    };
    let result = state
        .db
        .write(move |tx| {
            idempotent(tx, &service.id, &path, &key, &digest, ctx.now, |tx| {
                let lobby = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
                if !visible(tx, &viewer, &lobby)? {
                    return Err(ApiFailure::not_found());
                }
                if lobby.revision != expected {
                    return Err(ApiFailure::new(ErrorCode::StaleRevision, "The lobby changed. Refresh it and try again.")
                        .detail("current_revision", lobby.revision.to_string()));
                }
                if !lobby.open {
                    return Err(closed());
                }
                if let Some(current) = &lobby.current_match_id {
                    matches::cancel_lobby_set(tx, &ctx, current, "lobby_closed")?;
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
                    &ctx,
                    &after,
                    Kind::LobbyClosed,
                    json!({ "lobby_id": lobby.id, "lobby_revision": revision.to_string(), "reason": command.reason }),
                )?;
                audit(tx, ctx.now, (actor, service.id.clone()), "lobby.close", &lobby.id, "ok", Some(&command.reason))?;
                Ok((StatusCode::OK, snapshot(tx, &after)?))
            })
        })
        .await?;
    state.committed();
    Ok(into_response(result))
}
