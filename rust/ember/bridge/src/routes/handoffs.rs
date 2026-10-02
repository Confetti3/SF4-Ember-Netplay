//! Browser handoffs (spec 12.1): a provider or organizer mints a one-use,
//! 60-second code that lets one assigned player open a match in Ember from a
//! browser button. The code is not a credential on its own: only the expected
//! player's identity can redeem it, with a proof made for this route, and
//! redeeming it only names the match. Joining still goes through the claim
//! and the room's own admission.
use axum::{
    extract::State,
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    api::ErrorCode,
    challenge::{Action, Method},
    encoding::b64u,
    play::{
        CreateHandoff, HANDOFF_SECS, HandoffCreated, HandoffRedeemed, RedeemHandoff, handoff_uri,
    },
};
use rusqlite::{OptionalExtension, Transaction, params};

use crate::{
    AppState, Keys, auth,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, PROOF_BODY, json as respond},
    routes::{
        ledger::{load, participants},
        links::Ctx,
        matches::{service_viewer, visible},
        play::{fighter, playing},
        sessions::{self, Target},
    },
    util::random,
};

/// Handoffs one player may hold for one match at a time.
const MAX_LIVE: i64 = 4;
pub const REDEEM_PATH: &str = "/v1/handoffs/redeem";

fn code_hash(keys: &Keys, code: &str) -> [u8; 32] {
    keys.keyed_hash("handoff", code.as_bytes())
}

/// `POST /v1/handoffs`.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let viewer = service_viewer(&service)?;
    let command: CreateHandoff = body.parse()?;
    command.check()?;
    let ctx = Ctx::of(&state);
    let created = state
        .db
        .write(move |tx| {
            let found = load(tx, &command.match_id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &found)? {
                return Err(ApiFailure::not_found());
            }
            if found.state.is_terminal() {
                return Err(
                    ApiFailure::new(ErrorCode::StaleRevision, "The match is finished.")
                        .detail("state", found.state.as_str()),
                );
            }
            let roster = participants(tx, &found.id, found.generation)?;
            if !roster.iter().any(|p| p.ember_id == command.ember_id) {
                return Err(ApiFailure::invalid(
                    "That Ember ID is not assigned to this match.",
                ));
            }
            // Played through Ember: an organizer-reported match has no room to open.
            fighter(tx, &found.id, &command.ember_id)?;
            mint(tx, &ctx, &found.id, &command)
        })
        .await?;
    state.committed();
    Ok(respond(StatusCode::CREATED, &created))
}

fn mint(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    match_id: &str,
    command: &CreateHandoff,
) -> Result<HandoffCreated> {
    tx.execute("DELETE FROM handoffs WHERE expires_at <= ?1", [ctx.now])?;
    let live: i64 = tx.query_row(
        "SELECT COUNT(*) FROM handoffs WHERE match_id = ?1 AND ember_id = ?2 AND redeemed_at IS NULL",
        params![match_id, command.ember_id.as_str()],
        |row| row.get(0),
    )?;
    if live >= MAX_LIVE {
        return Err(ApiFailure::rate_limited(HANDOFF_SECS));
    }
    let code = b64u(&random::<32>());
    let expires_at = ctx.now + HANDOFF_SECS;
    tx.execute(
        "INSERT INTO handoffs (code_hash, match_id, ember_id, created_at, expires_at)
         VALUES (?1, ?2, ?3, ?4, ?5)",
        params![
            code_hash(&ctx.keys, &code).as_slice(),
            match_id,
            command.ember_id.as_str(),
            ctx.now,
            expires_at
        ],
    )?;
    Ok(HandoffCreated {
        uri: handoff_uri(&ctx.config.bridge_id, &code),
        handoff: code,
        bridge_id: ctx.config.bridge_id.clone(),
        match_id: match_id.to_owned(),
        expires_at,
    })
}

/// `POST /v1/handoffs/redeem`: a `handoff.redeem` proof.
pub async fn redeem_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = playing(&state, &headers).await?;
    let (request, command): (_, RedeemHandoff) = sessions::proven(&body.0)?;
    command.check()?;
    let ctx = Ctx::of(&state);
    let redeemed = state
        .db
        .write(move |tx| {
            let (ember_id, _) = sessions::consume_proof(
                tx,
                &ctx.config,
                ctx.now,
                &request,
                Target {
                    action: Action::HandoffRedeem,
                    method: Method::Post,
                    path: REDEEM_PATH,
                },
                Some(&player.ember_id),
            )?;
            // Unknown, expired, used, or for another player all read the
            // same: the code says nothing to anyone but its own player.
            let hash = code_hash(&ctx.keys, &command.handoff);
            let used = tx.execute(
                "UPDATE handoffs SET redeemed_at = ?1
                 WHERE code_hash = ?2 AND ember_id = ?3 AND redeemed_at IS NULL AND expires_at > ?1",
                params![ctx.now, hash.as_slice(), ember_id.as_str()],
            )?;
            if used != 1 {
                return Err(ApiFailure::not_found());
            }
            let match_id: String = tx
                .query_row(
                    "SELECT match_id FROM handoffs WHERE code_hash = ?1",
                    [hash.as_slice()],
                    |row| row.get(0),
                )
                .optional()?
                .ok_or_else(ApiFailure::not_found)?;
            // The match may have finished since the code was made.
            let found = load(tx, &match_id)?.ok_or_else(ApiFailure::not_found)?;
            if found.state.is_terminal() {
                return Err(ApiFailure::new(ErrorCode::StaleRevision, "The match is finished.")
                    .detail("state", found.state.as_str()));
            }
            Ok(HandoffRedeemed { match_id })
        })
        .await?;
    state.committed();
    Ok(respond(StatusCode::OK, &redeemed))
}

/// Drops handoffs long past use.
pub fn expire(tx: &Transaction<'_>, cutoff: u64) -> Result<()> {
    tx.execute("DELETE FROM handoffs WHERE expires_at <= ?1", [cutoff])?;
    Ok(())
}
