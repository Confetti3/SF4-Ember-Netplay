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
    challenge::Action,
    play::{WaitReason, WatchAnswer, WatchRequest},
};
use rusqlite::{Transaction, params};
use serde::Deserialize;

pub use organizer::{
    clear_denial, connection_policy, deny, match_watchers, remove_staff, set_link_holders,
    set_staff,
};
pub use rewatch::{bound, named};

use self::{
    access::{Admission, Policy},
    rewatch::{attested, fighter_claims, live_after},
};
use crate::{
    AppState,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json as respond},
    routes::{
        play::{claims, endpoints, finished, invitation, played, rebind, room},
        sessions::{Call, Rate, proven_call},
    },
};

/// A watcher asks again this often to stay a watcher.
const RETRY_SECS: u64 = 10;
/// Watch calls one Ember ID may make per window.
const RATE: Rate = Rate {
    scope: "match-watch",
    calls: 30,
    window_secs: 60,
};

/// A refusal with the reason clients act on, if it has one.
fn refuse(code: ErrorCode, reason: Option<&'static str>, message: &'static str) -> ApiFailure {
    let failure = ApiFailure::new(code, message);
    match reason {
        Some(reason) => failure.detail("reason", reason),
        None => failure,
    }
}

/// How watchers are told the match is over.
fn finished_reason(_: &crate::routes::ledger::Match) -> (&'static str, &'static str) {
    ("reason", "finished")
}

/// `POST /v1/matches/{id}/watch`: a `match.watch` proof.
pub async fn watch_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let call = Call::new(Action::MatchWatch, format!("/v1/matches/{id}/watch")).limited(RATE);
    let answer = proven_call(
        &state,
        &headers,
        &body,
        call,
        WatchRequest::check,
        move |tx, ctx, ember_id, command: WatchRequest| watch(tx, ctx, &id, ember_id, &command),
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
    let mut game = played(tx, match_id)?;
    finished(&game.found, finished_reason)?;
    if game.roster.iter().any(|p| p.ember_id == *ember_id) {
        return Err(refuse(
            ErrorCode::Forbidden,
            Some("fighter"),
            "You play in this match.",
        ));
    }
    match Policy::load(tx, &game.found.connection_id, match_id)?.admission(ember_id) {
        Admission::Denied => {
            return Err(refuse(
                ErrorCode::Forbidden,
                Some("revoked"),
                "The organizer removed you from this match's watchers.",
            ));
        }
        Admission::Closed => {
            return Err(refuse(
                ErrorCode::Forbidden,
                Some("not_allowed"),
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
            None,
            "The players run a different Ember build. Watching needs the same one.",
        ));
    }
    // A watcher who came back after going quiet is a new arrival: it does not
    // take the place of one who took its own in the meantime.
    tx.execute(
        "INSERT INTO match_watchers (match_id, ember_id, endpoint_id, helper_instance_id, build_id, first_seen_at, last_seen_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?6)
         ON CONFLICT (match_id, ember_id) DO UPDATE SET endpoint_id = ?3, helper_instance_id = ?4, build_id = ?5,
            first_seen_at = CASE WHEN match_watchers.last_seen_at > ?7 THEN match_watchers.first_seen_at ELSE ?6 END,
            last_seen_at = ?6",
        params![match_id, ember_id.as_str(), command.endpoint_id, command.helper_instance_id, command.build_id, ctx.now, live_after(ctx.now)],
    )?;
    rebind(tx, ctx, &mut game, &claimed)?;
    let current = room(tx, match_id)?;
    let waiting = |reason| WatchAnswer::Waiting {
        retry_after: RETRY_SECS,
        reason,
    };
    if current.room_id.is_none() {
        return Ok(waiting(WaitReason::NoRoom));
    }
    // The binding can name a watcher only once both fighters have claimed and
    // both say their helpers read watchers.
    if endpoints(&game, &claimed).is_none() || !attested(&fighter_claims(&game, &claimed)) {
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
    let call = Call::new(
        Action::MatchUnwatch,
        format!("/v1/matches/{id}/watch/leave"),
    )
    .limited(RATE);
    proven_call(
        &state,
        &headers,
        &body,
        call,
        |_: &Nothing| Ok(()),
        move |tx, ctx, ember_id, _: Nothing| {
            let mut game = played(tx, &id)?;
            tx.execute(
                "DELETE FROM match_watchers WHERE match_id = ?1 AND ember_id = ?2",
                params![id, ember_id.as_str()],
            )?;
            rebind(tx, ctx, &mut game, &claims(tx, &id)?)
        },
    )
    .await?;
    Ok(respond(
        StatusCode::OK,
        &serde_json::json!({ "left": true }),
    ))
}
