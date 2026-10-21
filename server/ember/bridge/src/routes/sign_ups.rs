//! Signing up for an organizer's event through Ember
//! (docs/design/TOURNAMENT_DESK.md). An organizer opens sign-ups on an event
//! they created on a site that takes participants (`organizer.participants`
//! in its definition), until it is ready. A player with that site's account
//! connected signs up from the game or the web desk, and the bridge adds
//! them to the site's event under their site username with the organizer's
//! grant. The bracket connector then finds them by that username, as it
//! finds anyone else, so it needs nothing of its own for them.
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    api::ErrorCode,
    challenge::{Action, Method},
};
use rusqlite::{OptionalExtension, params};
use serde_json::json;

use crate::{
    AppState,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json, ok},
    linking::Site,
    routes::{
        organizer::{Caller, site_name},
        sessions,
        sign_in::{OAuth, organizer_grant, spend},
    },
};

/// The path a player signs up for event `id` at.
pub fn path(id: &str) -> String {
    format!("/v1/events/{id}/sign-up")
}

/// An event taking sign-ups: its organizer, site and the site's event.
struct Open {
    organizer: String,
    site: String,
    tournament: String,
}

/// `POST /v1/events/{id}/sign-up`: signs the session player (or the one
/// signed in to the web desk) up for the event, and adds them to the site's
/// event as a participant. Signing up again changes nothing; one whose
/// adding failed is added again. Takes a `site.connect` proof for this path.
pub async fn sign_up(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, sessions::Nothing {}) = Caller::of(&state, &headers, &body).await?;
    let path = path(&id);
    let ctx = Ctx::of(&state);
    let event = id.clone();
    let signed = state
        .db
        .write(move |tx| {
            let open: Option<Open> = tx
                .query_row(
                    "SELECT ember_id, site, tournament FROM organizer_events
                     WHERE id = ?1 AND sign_ups = 1 AND status = 'draft'
                         AND tournament IS NOT NULL AND ended_at IS NULL",
                    [&event],
                    |row| {
                        Ok(Open {
                            organizer: row.get(0)?,
                            site: row.get(1)?,
                            tournament: row.get(2)?,
                        })
                    },
                )
                .optional()?;
            let Some(open) = open else {
                return Ok(Err("Sign-ups for this event are closed."));
            };
            let site = Site::Configured(open.site.clone());
            let ember_id = caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, &path)?;
            let Some(account) = site.account(tx, &ember_id)? else {
                return Ok(Err(
                    "Connect your account on the event's site in Ember first.",
                ));
            };
            tx.execute(
                "INSERT OR IGNORE INTO event_sign_ups (event_id, ember_id, username, signed_up_at)
                 VALUES (?1, ?2, ?3, ?4)",
                params![event, ember_id, account.username, ctx.now],
            )?;
            let (username, participant): (String, Option<String>) = tx.query_row(
                "SELECT username, participant FROM event_sign_ups
                 WHERE event_id = ?1 AND ember_id = ?2",
                params![event, ember_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?;
            Ok(Ok((open, ember_id, username, participant)))
        })
        .await?;
    state.committed();
    let (open, ember_id, username, participant) = signed.map_err(ApiFailure::invalid)?;
    if participant.is_some() {
        return Ok(ok(&json!({ "signed_up": true })));
    }
    let site = Site::Configured(open.site);
    let oauth = OAuth::organizer(&state, &site)
        .filter(OAuth::takes_sign_ups)
        .ok_or_else(ApiFailure::not_found)?;
    let grant = organizer_grant(&state, &site, &open.organizer)
        .await?
        .ok_or_else(ApiFailure::unavailable)?;
    if let Some(limits) = oauth.limits
        && !spend(&state, site.id(), &ember_id, 1, limits).await?
    {
        return Err(ApiFailure::sign_in_busy(None));
    }
    // ponytail: two sign-ups of one player at once may both add them; one
    // per player at a time if a site shows doubles.
    let added = oauth
        .add_participant(
            &grant.access,
            &open.tournament,
            &username,
            &username,
            &format!("ember:{ember_id}"),
        )
        .await
        .map_err(|refused| {
            let Some(said) = refused else {
                return ApiFailure::unavailable();
            };
            eprintln!("sign-up: {} refused a participant: {said}", site.id());
            ApiFailure::new(
                ErrorCode::InvalidRequest,
                format!(
                    "{} refused the sign-up: {said}",
                    site_name(&state, site.id())
                ),
            )
            .detail("reason", "site_refused")
            .detail("site_said", said)
        })?;
    state
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE event_sign_ups SET participant = ?1
                 WHERE event_id = ?2 AND ember_id = ?3 AND participant IS NULL",
                params![added, id, ember_id],
            )?;
            Ok(())
        })
        .await?;
    state.committed();
    Ok(json(StatusCode::CREATED, &json!({ "signed_up": true })))
}

/// `DELETE /v1/events/{id}/sign-up`: takes the player's sign-up back, and
/// them out of the site's event, while the event has not started. Takes a
/// `site.remove` proof for this path.
pub async fn withdraw(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, sessions::Nothing {}) = Caller::of(&state, &headers, &body).await?;
    let path = path(&id);
    let ctx = Ctx::of(&state);
    let event = id.clone();
    let found = state
        .db
        .write(move |tx| {
            let ember_id = caller.prove(tx, &ctx, Action::SiteRemove, Method::Delete, &path)?;
            let row: Option<(String, String, String, String, Option<String>)> = tx
                .query_row(
                    "SELECT e.status, e.ember_id, e.site, e.tournament, s.participant
                     FROM event_sign_ups s JOIN organizer_events e ON e.id = s.event_id
                     WHERE s.event_id = ?1 AND s.ember_id = ?2",
                    params![event, ember_id],
                    |row| {
                        Ok((
                            row.get(0)?,
                            row.get(1)?,
                            row.get(2)?,
                            row.get(3)?,
                            row.get(4)?,
                        ))
                    },
                )
                .optional()?;
            Ok(row.map(|row| (ember_id, row)))
        })
        .await?;
    state.committed();
    let (ember_id, (status, organizer, site, tournament, participant)) =
        found.ok_or_else(ApiFailure::not_found)?;
    if status != "draft" {
        return Err(ApiFailure::invalid(
            "The event has started; ask its organizer to take you out.",
        ));
    }
    if let Some(participant) = participant {
        let site = Site::Configured(site);
        let oauth = OAuth::organizer(&state, &site).ok_or_else(ApiFailure::not_found)?;
        let grant = organizer_grant(&state, &site, &organizer)
            .await?
            .ok_or_else(ApiFailure::unavailable)?;
        if let Some(limits) = oauth.limits
            && !spend(&state, site.id(), &ember_id, 1, limits).await?
        {
            return Err(ApiFailure::sign_in_busy(None));
        }
        // Kept until the site says it took them out, so trying again can.
        if !oauth
            .remove_participant(&grant.access, &tournament, &participant)
            .await
        {
            return Err(ApiFailure::unavailable());
        }
    }
    state
        .db
        .write(move |tx| {
            tx.execute(
                "DELETE FROM event_sign_ups WHERE event_id = ?1 AND ember_id = ?2",
                params![id, ember_id],
            )?;
            Ok(())
        })
        .await?;
    state.committed();
    Ok(ok(&json!({ "signed_up": false })))
}
