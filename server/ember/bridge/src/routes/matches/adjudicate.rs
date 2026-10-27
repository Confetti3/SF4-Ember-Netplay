//! `POST /v1/matches/{id}/adjudications`: an organizer decides or voids a
//! game.
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    api::ErrorCode,
    encoding::{Counter, is_prefixed_id},
    event::{GameConfirmedData, Kind},
    matches::{MatchState, Resolution},
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};
use serde_json::json;

use super::{check_reason, read::visible};
use crate::{
    AppState,
    audit::audit,
    auth::{self, Role, Service},
    ctx::Ctx,
    error::{ApiFailure, Result},
    events::Viewer,
    http::{Body, GENERAL_BODY, expected_revision, idempotency_key},
    routes::{
        idempotency::ServiceCall,
        ledger::{Cause, Match, load, participants, scores, settle, stale, unready_before},
    },
};

const MAX_EVIDENCE: usize = 16;

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
    let viewer = Viewer::of_service(&service)?;
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
    let path = format!("/v1/matches/{id}/adjudications");
    ServiceCall::new(path)
        .run(
            &state,
            service.id.clone(),
            key,
            command,
            move |tx, ctx, _, command| {
                let found = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
                if !visible(tx, &viewer, &found)? {
                    return Err(ApiFailure::not_found());
                }
                if found.revision != expected {
                    return Err(stale(found.revision));
                }
                apply_adjudication(tx, ctx, &service, &found, &command, expected)
            },
        )
        .await
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
                    unready_before(tx, &found.id, &open_id)?;
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
                serde_json::to_value(GameConfirmedData {
                    match_id: found.id.clone(),
                    match_revision: None,
                    attempt_id,
                    seq,
                    outcome: outcome.into(),
                    winner_id: winner,
                    resolution: Some(Resolution::OrganizerAdjudication),
                    adjudication_id: Some(adjudication_id.clone()),
                    report_ids: None,
                })
                .unwrap_or_default(),
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
                    "A cancelled, failed or expired match cannot change.",
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
