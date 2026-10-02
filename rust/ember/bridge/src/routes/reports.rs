//! Fighters' signed game reports and their reconciliation (spec 16).
//!
//! A report is stored once per (attempt, reporter, observation) and kept as
//! evidence whatever happens next. Each fighter's first report for an attempt
//! is the one that counts. Two that agree decide the attempt and the ledger
//! scores it; two that disagree, or one left alone for a minute, hold the
//! attempt for an organizer.
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    api::ErrorCode,
    event::Kind,
    json,
    matches::MatchState,
    play::SignedPermit,
    report::{GameReport, MAX_REPORT_BODY, Outcome, SignedReport},
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde_json::json;

use crate::{
    AppState, auth,
    error::{ApiFailure, Result},
    http::{Body, json as respond},
    routes::{
        ledger::{Cause, Match, bump, load, match_event, participants, scores, settle},
        links::Ctx,
        play::{self, fighter},
    },
    util::new_id,
};

/// How long one report waits for the other before review (spec 16.6).
pub const PARTNER_SECS: u64 = 60;
/// Automatic scoring window from the attempt's permit (spec 16.7).
pub const SCORING_SECS: u64 = 24 * 60 * 60;
/// A permitted game with no report at all goes to review after this.
pub const SILENT_SECS: u64 = 30 * 60;

struct Attempt {
    id: String,
    state: String,
    permit: SignedPermit,
    created_at: u64,
}

fn attempt(tx: &Transaction<'_>, match_id: &str, permit_id: &str) -> Result<Option<Attempt>> {
    tx.query_row(
        "SELECT id, state, permit, created_at FROM attempts WHERE match_id = ?1 AND permit_id = ?2",
        params![match_id, permit_id],
        |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, String>(2)?,
                row.get::<_, u64>(3)?,
            ))
        },
    )
    .optional()?
    .map(|(id, state, permit, created_at)| {
        Ok(Attempt {
            id,
            state,
            permit: serde_json::from_str(&permit).map_err(|_| ApiFailure::unavailable())?,
            created_at,
        })
    })
    .transpose()
}

/// `POST /v1/matches/{id}/reports`: a fighter's session and a signed report,
/// its own or the other fighter's, forwarded unchanged.
pub async fn submit_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<MAX_REPORT_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    if !player.scopes.iter().any(|scope| scope == play::SCOPE) {
        return Err(ApiFailure::forbidden());
    }
    let signed: SignedReport = body.parse()?;
    signed.verify().map_err(|_| {
        ApiFailure::new(
            ErrorCode::InvalidSignature,
            "The report's signature or fields do not check out.",
        )
    })?;
    let ctx = Ctx::of(&state);
    let (status, receipt) = state
        .db
        .write(move |tx| {
            // Only the match's own fighters submit; the signature says whose report it is.
            fighter(tx, &id, &player.ember_id)?;
            submit(tx, &ctx, &id, &signed)
        })
        .await?;
    state.committed();
    Ok(respond(status, &receipt))
}

fn submit(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    match_id: &str,
    signed: &SignedReport,
) -> Result<(StatusCode, serde_json::Value)> {
    let report = &signed.report;
    let found = attempt(tx, match_id, &report.permit_id)?
        .filter(|found| found.permit.permit.covers(report))
        .ok_or_else(|| {
            ApiFailure::new(
                ErrorCode::StaleRevision,
                "That report is not about a game this match permitted.",
            )
            .detail("reason", "unknown_attempt")
        })?;
    let digest = json::digest(signed)?;
    let stored: Option<(String, String)> = tx
        .query_row(
            "SELECT id, digest FROM game_reports WHERE attempt_id = ?1 AND reporter_id = ?2 AND observation_id = ?3",
            params![found.id, report.reporter_id.as_str(), report.observation_id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?;
    if let Some((report_id, stored_digest)) = stored {
        // An exact retry gets the original receipt; the same observation
        // with different contents is refused and the first one stands.
        if stored_digest != digest {
            return Err(ApiFailure::new(
                ErrorCode::IdempotencyConflict,
                "A different report with this observation ID was already received.",
            ));
        }
        return Ok((
            StatusCode::OK,
            receipt(tx, match_id, &report_id, &found.id)?,
        ));
    }
    let report_id = new_id("rpt");
    tx.execute(
        "INSERT INTO game_reports (id, attempt_id, match_id, reporter_id, observation_id, digest, signed, result, received_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        params![
            report_id,
            found.id,
            match_id,
            report.reporter_id.as_str(),
            report.observation_id,
            digest,
            serde_json::to_string(signed).unwrap_or_default(),
            outcome_name(report.result),
            ctx.now
        ],
    )?;
    // Evidence only once the attempt is decided, or while the match is
    // finished or waiting for an organizer; past the window it goes to review.
    let current = load(tx, match_id)?.ok_or_else(ApiFailure::unavailable)?;
    let open = found.state == "permitted"
        && !current.state.is_terminal()
        && current.state != MatchState::NeedsReview;
    if open && ctx.now <= found.created_at + SCORING_SECS {
        reconcile(tx, ctx, match_id, &found)?;
    } else if open {
        hold(tx, ctx, match_id, &found.id, "report_late")?;
    }
    Ok((
        StatusCode::CREATED,
        receipt(tx, match_id, &report_id, &found.id)?,
    ))
}

fn receipt(
    tx: &Transaction<'_>,
    match_id: &str,
    report_id: &str,
    attempt_id: &str,
) -> Result<serde_json::Value> {
    let (attempt_state, match_state): (String, String) = tx.query_row(
        "SELECT a.state, m.state FROM attempts a JOIN matches m ON m.id = a.match_id WHERE a.id = ?1 AND m.id = ?2",
        params![attempt_id, match_id],
        |row| Ok((row.get(0)?, row.get(1)?)),
    )?;
    Ok(json!({
        "report_id": report_id,
        "attempt_id": attempt_id,
        "attempt_state": attempt_state,
        "match_state": match_state,
    }))
}

fn outcome_name(outcome: Outcome) -> &'static str {
    match outcome {
        Outcome::P1Win => "p1_win",
        Outcome::P2Win => "p2_win",
        Outcome::Draw => "draw",
        Outcome::Abort => "abort",
        Outcome::Cancel => "cancel",
    }
}

/// Each fighter's first report for the attempt, by reporter.
fn first_reports(tx: &Transaction<'_>, attempt_id: &str) -> Result<Vec<(String, GameReport)>> {
    let rows = tx
        .prepare(
            "SELECT r.id, r.signed FROM game_reports r
              WHERE r.attempt_id = ?1 AND r.id = (
                SELECT f.id FROM game_reports f WHERE f.attempt_id = r.attempt_id AND f.reporter_id = r.reporter_id
                 ORDER BY f.received_at, f.rowid LIMIT 1)",
        )?
        .query_map([attempt_id], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?)))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    rows.into_iter()
        .map(|(id, text)| {
            let signed: SignedReport =
                serde_json::from_str(&text).map_err(|_| ApiFailure::unavailable())?;
            Ok((id, signed.report))
        })
        .collect()
}

fn reconcile(tx: &Transaction<'_>, ctx: &Ctx, match_id: &str, found: &Attempt) -> Result<()> {
    let reports = first_reports(tx, &found.id)?;
    let current = load(tx, match_id)?.ok_or_else(ApiFailure::unavailable)?;
    let [(first_id, first), (second_id, second)] = reports.as_slice() else {
        // One report so far: wait for the other fighter's.
        tx.execute(
            "UPDATE attempts SET first_report_at = COALESCE(first_report_at, ?1) WHERE id = ?2",
            params![ctx.now, found.id],
        )?;
        let revision = if current.state == MatchState::Running {
            bump(tx, &current, MatchState::AwaitingReports, ctx.now)?
        } else {
            current.revision
        };
        let report = &reports[0].1;
        match_event(
            tx,
            ctx,
            &current,
            Kind::GameReported,
            json!({
                "match_id": match_id,
                "match_revision": revision.to_string(),
                "attempt_id": found.id,
                "reporter_id": report.reporter_id,
                "result": report.result,
            }),
        )?;
        return Ok(());
    };
    if !first.agrees_with(second) {
        return hold(tx, ctx, match_id, &found.id, "reports_disagree");
    }
    decide(
        tx,
        ctx,
        &current,
        &found.id,
        first.result,
        vec![first_id.clone(), second_id.clone()],
    )
}

/// Two agreeing reports decide the attempt. A win or draw is accepted; an
/// agreed abort or cancel counts for nobody (spec 16.5).
fn decide(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    current: &Match,
    attempt_id: &str,
    result: Outcome,
    report_ids: Vec<String>,
) -> Result<()> {
    let roster = participants(tx, &current.id, current.generation)?;
    let (wins_before, _) = scores(tx, &current.id)?;
    let seq: u64 = tx.query_row(
        "SELECT seq FROM attempts WHERE id = ?1",
        [attempt_id],
        |row| row.get(0),
    )?;
    let mut events = Vec::new();
    if result.is_native_result() {
        tx.execute(
            "UPDATE attempts SET state = 'accepted', outcome = ?1 WHERE id = ?2 AND state = 'permitted'",
            params![outcome_name(result), attempt_id],
        )?;
        let winner = match result {
            Outcome::P1Win => Some(roster[0].ember_id.clone()),
            Outcome::P2Win => Some(roster[1].ember_id.clone()),
            _ => None,
        };
        events.push((
            Kind::GameConfirmed,
            json!({
                "match_id": current.id,
                "attempt_id": attempt_id,
                "seq": seq,
                "outcome": outcome_name(result),
                "winner_id": winner,
                "resolution": ember_protocol::matches::Resolution::PlayerAgreement,
                "report_ids": report_ids,
            }),
        ));
    } else {
        tx.execute(
            "UPDATE attempts SET state = 'aborted' WHERE id = ?1 AND state = 'permitted'",
            [attempt_id],
        )?;
    }
    settle(
        tx,
        ctx,
        current,
        &roster,
        wins_before,
        Cause::Players { report_ids },
        events,
    )?;
    Ok(())
}

/// Holds an attempt for an organizer and blocks the next game (spec 16.6).
fn hold(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    match_id: &str,
    attempt_id: &str,
    reason: &str,
) -> Result<()> {
    let changed = tx.execute(
        "UPDATE attempts SET state = 'review' WHERE id = ?1 AND state = 'permitted'",
        [attempt_id],
    )?;
    let current = load(tx, match_id)?.ok_or_else(ApiFailure::unavailable)?;
    if changed == 0 || current.state.is_terminal() || current.state == MatchState::NeedsReview {
        return Ok(());
    }
    let revision = bump(tx, &current, MatchState::NeedsReview, ctx.now)?;
    match_event(
        tx,
        ctx,
        &current,
        Kind::NeedsReview,
        json!({
            "match_id": match_id,
            "match_revision": revision.to_string(),
            "state": MatchState::NeedsReview,
            "attempt_id": attempt_id,
            "reason": reason,
        }),
    )?;
    Ok(())
}

/// Holds attempts whose second report never came, and permitted games that
/// nobody reported at all. Runs with the bridge's other maintenance.
pub fn expire(tx: &Transaction<'_>, ctx: &Ctx) -> Result<()> {
    let due = tx
        .prepare(
            "SELECT match_id, id, CASE WHEN first_report_at IS NULL THEN 'no_reports' ELSE 'report_missing' END
               FROM attempts
              WHERE state = 'permitted'
                AND ((first_report_at IS NOT NULL AND first_report_at <= ?1)
                  OR (first_report_at IS NULL AND start_by <= ?2))",
        )?
        .query_map(
            params![ctx.now.saturating_sub(PARTNER_SECS), ctx.now.saturating_sub(SILENT_SECS)],
            |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?, row.get::<_, String>(2)?)),
        )?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for (match_id, attempt_id, reason) in due {
        hold(tx, ctx, &match_id, &attempt_id, &reason)?;
    }
    Ok(())
}
