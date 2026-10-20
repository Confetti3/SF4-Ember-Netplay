//! Watching a bridge match (docs/design/MATCH_WATCH.md). A player who is not
//! one of its fighters asks to watch with a proof for their own Ember ID.
//! When the organizer's policy allows it and the bridge names them in the
//! binding, they are answered with the room and its invitation; the fighters'
//! next claims carry the binding, which is how the room learns to admit them.
//! A watcher is never given a binding.
mod access;
mod organizer;
mod rewatch;

use std::borrow::Cow;

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

pub use organizer::{
    clear_denial, connection_policy, deny, match_watchers, remove_staff, set_link_holders,
    set_staff,
};
pub(super) use rewatch::{Named, named};

use self::{
    access::{Admission, WatchPolicy},
    rewatch::{fighter_claims, live_after, on_fighters_build},
};
use super::{
    finished, playing, rebind,
    room::{claims, invitation, room},
    roster_of, with_rules,
};
use crate::{
    AppState,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json as respond},
    routes::sessions::{Call, Nothing, Rate},
};

/// A watcher asks again this often to stay a watcher.
const RETRY_SECS: u64 = 10;
/// Watch calls one Ember ID may make per window.
const RATE: Rate = Rate {
    scope: Cow::Borrowed("match-watch"),
    calls: 30,
    window_secs: 60,
};

/// `POST /v1/matches/{id}/watch`: a `match.watch` proof.
pub async fn watch_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = playing(&state, &headers).await?;
    let call = Call::new(Action::MatchWatch, format!("/v1/matches/{id}/watch")).limited(RATE);
    let answer = call
        .run(
            &state,
            &player,
            &body,
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
    // A stranger is told what the watch policy says before anything about the
    // match's rules.
    let (found, roster) = roster_of(tx, match_id)?;
    if roster.iter().any(|p| p.ember_id == *ember_id) {
        return Err(
            ApiFailure::new(ErrorCode::Forbidden, "You play in this match.")
                .detail("reason", "fighter"),
        );
    }
    match WatchPolicy::load(tx, &found.connection_id, match_id)?.admission(ember_id) {
        Admission::Denied => {
            return Err(ApiFailure::new(
                ErrorCode::Forbidden,
                "The organizer removed you from this match's watchers.",
            )
            .detail("reason", "revoked"));
        }
        Admission::Closed => {
            return Err(ApiFailure::new(
                ErrorCode::Forbidden,
                "This match's organizer has not opened it to watchers.",
            )
            .detail("reason", "not_allowed"));
        }
        Admission::Staff | Admission::LinkHolder => {}
    }
    // Only someone admitted learns how a finished match ended.
    finished(&found)?;
    let mut game = with_rules(tx, found, roster)?;
    let claimed = claims(tx, match_id)?;
    if !on_fighters_build(&fighter_claims(&game, &claimed), &command.build_id) {
        return Err(ApiFailure::new(
            ErrorCode::IncompatibleBuild,
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
    let waiting = |reason| WatchAnswer::Waiting {
        retry_after: RETRY_SECS,
        reason,
    };
    // The one derivation of the binding's set, which every claim makes too.
    Ok(match rebind(tx, ctx, &mut game, &claimed)? {
        Named::NoRoom => waiting(WaitReason::NoRoom),
        Named::NotBound { .. } => waiting(WaitReason::NotBound),
        Named::Set { watchers, .. } if watchers.iter().all(|w| w.ember_id != *ember_id) => {
            waiting(WaitReason::Full)
        }
        Named::Set { .. } => {
            let (room_id, invitation) = invitation(ctx, &room(tx, match_id)?)?;
            WatchAnswer::Room {
                room_id,
                invitation,
            }
        }
    })
}

/// `POST /v1/matches/{id}/watch/leave`: a `match.unwatch` proof.
pub async fn leave_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = playing(&state, &headers).await?;
    let call = Call::new(
        Action::MatchUnwatch,
        format!("/v1/matches/{id}/watch/leave"),
    )
    .limited(RATE);
    call.run(
        &state,
        &player,
        &body,
        Nothing::check,
        move |tx, ctx, ember_id, _: Nothing| {
            let left = tx.execute(
                "DELETE FROM match_watchers WHERE match_id = ?1 AND ember_id = ?2",
                params![id, ember_id.as_str()],
            )?;
            // Only a watcher's leaving changes the binding, and a finished
            // match's is no longer kept.
            if left == 0 {
                return Ok(());
            }
            let (found, roster) = roster_of(tx, &id)?;
            if found.state.is_terminal() {
                return Ok(());
            }
            let mut game = with_rules(tx, found, roster)?;
            rebind(tx, ctx, &mut game, &claims(tx, &id)?)?;
            Ok(())
        },
    )
    .await?;
    Ok(respond(
        StatusCode::OK,
        &serde_json::json!({ "left": true }),
    ))
}
