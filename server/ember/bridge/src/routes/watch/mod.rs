//! Watching a bridge match (docs/design/MATCH_WATCH.md). A player who is not
//! one of its fighters asks to watch with a proof for their own Ember ID.
//! When the organizer's policy allows it and the bridge names them in the
//! binding, they are answered with the room and its invitation; the fighters'
//! next claims carry the binding, which is how the room learns to admit them.
//! A watcher is never given a binding.
mod access;
mod organizer;
mod rewatch;

use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    challenge::{Action, Method},
    play::{WaitReason, WatchAnswer, WatchRequest},
};
use rusqlite::{Transaction, params};
use serde::Deserialize;

pub use organizer::{
    clear_denial, connection_policy, deny, match_watchers, remove_staff, set_link_holders,
    set_staff,
};
pub use rewatch::{bound, rewatch};

use self::access::{Admission, Policy};
use crate::{
    AppState,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json as respond},
    routes::{
        ledger::Match,
        play::{claims, endpoints, invitation, played, playing, room},
        sessions::{self, Target},
    },
};

/// A watcher asks again this often to stay a watcher.
const RETRY_SECS: u64 = 10;
/// Watch calls one Ember ID may make per window.
const CALLS_PER_WINDOW: usize = 30;
const WINDOW_SECS: u64 = 60;

/// A refusal with the reason clients act on.
fn refuse(code: ErrorCode, reason: &'static str, message: &'static str) -> ApiFailure {
    let failure = ApiFailure::new(code, message);
    if reason.is_empty() {
        failure
    } else {
        failure.detail("reason", reason)
    }
}

fn finished(found: &Match) -> Result<()> {
    if found.state.is_terminal() {
        return Err(refuse(
            ErrorCode::StaleRevision,
            "finished",
            "The match is finished.",
        ));
    }
    Ok(())
}

/// What a proven watch call is: its action, the route's path and what it does.
async fn proven_call<C: serde::de::DeserializeOwned + Send + 'static, T: Send + 'static>(
    state: &AppState,
    headers: &HeaderMap,
    body: &Body<PROOF_BODY>,
    action: Action,
    path: String,
    work: impl FnOnce(&Transaction<'_>, &Ctx, &EmberId, C) -> Result<T> + Send + 'static,
) -> Result<T> {
    let player = playing(state, headers).await?;
    let ctx = Ctx::of(state);
    state
        .rate
        .check(
            &format!("match-watch:{}", player.ember_id.as_str()),
            CALLS_PER_WINDOW,
            WINDOW_SECS,
            ctx.now,
        )
        .map_err(ApiFailure::rate_limited)?;
    let (request, command): (_, C) = sessions::proven(&body.0)?;
    let answer = state
        .db
        .write(move |tx| {
            let (ember_id, _) = sessions::consume_proof(
                tx,
                &ctx.config,
                ctx.now,
                &request,
                Target {
                    action,
                    method: Method::Post,
                    path: &path,
                },
                Some(&player.ember_id),
            )?;
            work(tx, &ctx, &ember_id, command)
        })
        .await?;
    state.committed();
    Ok(answer)
}

/// `POST /v1/matches/{id}/watch`: a `match.watch` proof.
pub async fn watch_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let path = format!("/v1/matches/{id}/watch");
    let answer = proven_call(
        &state,
        &headers,
        &body,
        Action::MatchWatch,
        path,
        move |tx, ctx, ember_id, command: WatchRequest| {
            command.check()?;
            watch(tx, ctx, &id, ember_id, &command)
        },
    )
    .await?;
    Ok(respond(StatusCode::OK, &answer))
}

fn watch(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    match_id: &str,
    ember_id: &EmberId,
    command: &WatchRequest,
) -> Result<WatchAnswer> {
    let fighter = played(tx, match_id)?;
    finished(&fighter.found)?;
    if fighter.roster.iter().any(|p| p.ember_id == *ember_id) {
        return Err(refuse(
            ErrorCode::Forbidden,
            "fighter",
            "You play in this match.",
        ));
    }
    match Policy::load(tx, &fighter.found.connection_id, match_id)?.admission(ember_id) {
        Admission::Denied => {
            return Err(refuse(
                ErrorCode::Forbidden,
                "revoked",
                "The organizer removed you from this match's watchers.",
            ));
        }
        Admission::Closed => {
            return Err(refuse(
                ErrorCode::Forbidden,
                "not_allowed",
                "This match's organizer has not opened it to watchers.",
            ));
        }
        Admission::Staff | Admission::LinkHolder => {}
    }
    let claimed = claims(tx, match_id)?;
    if claimed
        .iter()
        .any(|claim| claim.build_id != command.build_id)
    {
        return Err(refuse(
            ErrorCode::IncompatibleBuild,
            "",
            "The players run a different Ember build. Watching needs the same one.",
        ));
    }
    tx.execute(
        "INSERT INTO match_watchers (match_id, ember_id, endpoint_id, helper_instance_id, build_id, first_seen_at, last_seen_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?6)
         ON CONFLICT (match_id, ember_id) DO UPDATE SET endpoint_id = ?3, helper_instance_id = ?4, build_id = ?5, last_seen_at = ?6",
        params![match_id, ember_id.as_str(), command.endpoint_id, command.helper_instance_id, command.build_id, ctx.now],
    )?;
    rewatch(tx, ctx, &fighter, &claimed)?;
    let current = room(tx, match_id)?;
    let waiting = |reason| WatchAnswer::Waiting {
        retry_after: RETRY_SECS,
        reason,
    };
    if current.room_id.is_none() {
        return Ok(waiting(WaitReason::NoRoom));
    }
    if endpoints(&fighter, &claimed).is_none() {
        return Ok(waiting(WaitReason::NotBound));
    }
    if !bound(current.bound_watchers.as_deref())?
        .iter()
        .any(|watcher| watcher.ember_id == *ember_id)
    {
        return Ok(waiting(WaitReason::Full));
    }
    let (room_id, invitation) = invitation(ctx, &current)?;
    Ok(WatchAnswer::Room {
        room_id,
        invitation,
    })
}

/// The body of a command with nothing in it.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Nothing {}

/// `POST /v1/matches/{id}/watch/leave`: a `match.unwatch` proof.
pub async fn leave_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let path = format!("/v1/matches/{id}/watch/leave");
    proven_call(
        &state,
        &headers,
        &body,
        Action::MatchUnwatch,
        path,
        move |tx, ctx, ember_id, _: Nothing| {
            let fighter = played(tx, &id)?;
            tx.execute(
                "DELETE FROM match_watchers WHERE match_id = ?1 AND ember_id = ?2",
                params![id, ember_id.as_str()],
            )?;
            rewatch(tx, ctx, &fighter, &claims(tx, &id)?)
        },
    )
    .await?;
    Ok(respond(
        StatusCode::OK,
        &serde_json::json!({ "left": true }),
    ))
}
