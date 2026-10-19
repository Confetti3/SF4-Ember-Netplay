//! Organizers' own events (docs/design/TOURNAMENT_DESK.md). An organizer
//! who holds an organizer grant on a sign-in site (`sign_in`) lists the
//! events they run there and has the bridge run one: the bracket connector
//! reads the running events from `/v1/desk/brackets` with the provider
//! credential of the configured desk connection, each with a fresh access
//! token, and runs them as it runs one from its own configuration.
//!
//! An organizer acts only on their own events, and runs only an event the
//! site lists for them, so their token cannot have Ember run someone else's.
//! An event whose organizer no longer holds a grant is not handed out.
use axum::{
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::challenge::{Action, Method};
use rusqlite::params;
use serde::Deserialize;
use serde_json::{Value, json};

use crate::{
    AppState, auth,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json, ok},
    linking::Site,
    routes::{
        sessions::{self, Target},
        sign_in::{OAuth, Tournament, organizer_grant, spend},
    },
    util::new_id,
};

pub const EVENTS_PATH: &str = "/v1/organizer/events";

/// Events one organizer runs at once.
const RUNNING_PER_ORGANIZER: u64 = 8;

/// The largest settings an event takes, as JSON.
const MAX_SETTINGS: usize = 4096;

/// The settings the bracket connector reads for an event: those of a
/// tournament in its own configuration that are not its site, event or
/// credential. The connector checks their values as it checks its own
/// file's; the bridge only keeps them to this shape.
const SETTINGS: [&str; 5] = [
    "rounds",
    "hold",
    "active_from",
    "active_until",
    "mark_underway",
];

#[derive(Deserialize)]
pub struct Listing {
    site: String,
}

/// `GET /v1/organizer/tournaments?site=`: the events the session player runs
/// on the site, as the site lists them with their organizer grant.
pub async fn tournaments(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(listing): Query<Listing>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let site = Site::Configured(listing.site);
    let tournaments = listed(&state, &site, player.ember_id.as_str()).await?;
    Ok(ok(&json!({ "tournaments": tournaments })))
}

/// The events `ember_id` runs on `site`: `not_found` without an organizer
/// grant there, `unavailable` when the site does not answer.
async fn listed(state: &AppState, site: &Site, ember_id: &str) -> Result<Vec<Tournament>> {
    let oauth = OAuth::organizer(state, site).ok_or_else(ApiFailure::not_found)?;
    let grant = organizer_grant(state, site, ember_id)
        .await?
        .ok_or_else(ApiFailure::not_found)?;
    if let Some(limits) = oauth.limits
        && !spend(state, site.id(), limits).await?
    {
        return Err(ApiFailure::sign_in_busy(None));
    }
    oauth
        .tournaments(&grant.access)
        .await
        .ok_or_else(ApiFailure::unavailable)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Start {
    site: String,
    tournament: String,
    #[serde(default)]
    settings: serde_json::Map<String, Value>,
}

/// `POST /v1/organizer/events`: runs one of the session player's events on
/// the desk connection. Takes a `site.connect` proof for this path.
pub async fn start(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    if state.config.desk.is_none() {
        return Err(ApiFailure::not_found());
    }
    let player = auth::player(&state, &headers).await?;
    let (request, command): (_, Start) = sessions::proven(&body.0)?;
    if !command
        .settings
        .keys()
        .all(|key| SETTINGS.contains(&key.as_str()))
        || Value::Object(command.settings.clone()).to_string().len() > MAX_SETTINGS
    {
        return Err(ApiFailure::invalid(
            "settings take rounds, hold, active_from, active_until and mark_underway, \
             in at most 4096 bytes.",
        ));
    }
    let site = Site::Configured(command.site.clone());
    let event = listed(&state, &site, player.ember_id.as_str())
        .await?
        .into_iter()
        .find(|event| event.id == command.tournament)
        .ok_or_else(|| ApiFailure::invalid("The site lists no such event of yours."))?;
    let settings = Value::Object(command.settings).to_string();
    let ctx = Ctx::of(&state);
    let id = new_id("oev");
    let started = id.clone();
    let done = state
        .db
        .write(move |tx| {
            let (ember_id, _) = sessions::consume_proof(
                tx,
                &ctx.config,
                ctx.now,
                &request,
                Target {
                    action: Action::SiteConnect,
                    method: Method::Post,
                    path: EVENTS_PATH,
                },
                Some(&player.ember_id),
            )?;
            let running: u64 = tx.query_row(
                "SELECT COUNT(*) FROM organizer_events WHERE ember_id = ?1 AND ended_at IS NULL",
                [ember_id.as_str()],
                |row| row.get(0),
            )?;
            if running >= RUNNING_PER_ORGANIZER {
                return Ok(Err("Stop one of your events first: 8 run at once."));
            }
            let inserted = tx.execute(
                "INSERT INTO organizer_events (id, ember_id, site, tournament, name, settings, created_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                 ON CONFLICT DO NOTHING",
                params![
                    started,
                    ember_id.as_str(),
                    site.id(),
                    event.id,
                    event.name,
                    settings,
                    ctx.now
                ],
            )?;
            Ok(if inserted == 1 {
                Ok(())
            } else {
                Err("This event already runs on Ember.")
            })
        })
        .await?;
    state.committed();
    done.map_err(ApiFailure::invalid)?;
    Ok(json(StatusCode::CREATED, &json!({ "id": id })))
}

/// `GET /v1/organizer/events`: the session player's running events, newest
/// first.
pub async fn list(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let events = state
        .db
        .read(move |tx| {
            let mut query = tx.prepare(
                "SELECT id, site, tournament, name, settings, created_at FROM organizer_events
                 WHERE ember_id = ?1 AND ended_at IS NULL ORDER BY created_at DESC, id",
            )?;
            let rows = query.query_map([player.ember_id.as_str()], |row| {
                Ok(json!({
                    "id": row.get::<_, String>(0)?,
                    "site": row.get::<_, String>(1)?,
                    "tournament": row.get::<_, String>(2)?,
                    "name": row.get::<_, String>(3)?,
                    "settings": settings(&row.get::<_, String>(4)?),
                    "created_at": row.get::<_, u64>(5)?,
                }))
            })?;
            Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
        })
        .await?;
    Ok(ok(&json!({ "events": events })))
}

/// `DELETE /v1/organizer/events/{id}`: stops one of the session player's
/// events. Takes a `site.remove` proof for this path.
pub async fn stop(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let path = format!("{EVENTS_PATH}/{id}");
    let target = (Action::SiteRemove, Method::Delete, path);
    let stopped = sessions::as_proven(&state, player, &body, target, move |tx, ctx, ember_id| {
        Ok(tx.execute(
            "UPDATE organizer_events SET ended_at = ?1
             WHERE id = ?2 AND ember_id = ?3 AND ended_at IS NULL",
            params![ctx.now, id, ember_id],
        )? == 1)
    })
    .await?;
    if !stopped {
        return Err(ApiFailure::not_found());
    }
    Ok(ok(&json!({})))
}

/// `GET /v1/desk/brackets`: every running event whose organizer still holds
/// a grant, with a fresh access token, for the desk connection's provider.
pub async fn brackets(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let desk = state
        .config
        .desk
        .as_ref()
        .ok_or_else(ApiFailure::not_found)?;
    if service.provider_connection()? != desk.connection {
        return Err(ApiFailure::forbidden());
    }
    let running: Vec<(String, String, String, String, String, String)> = state
        .db
        .read(|tx| {
            let mut query = tx.prepare(
                "SELECT id, ember_id, site, tournament, name, settings FROM organizer_events
                 WHERE ended_at IS NULL ORDER BY created_at, id",
            )?;
            let rows = query.query_map([], |row| {
                Ok((
                    row.get(0)?,
                    row.get(1)?,
                    row.get(2)?,
                    row.get(3)?,
                    row.get(4)?,
                    row.get(5)?,
                ))
            })?;
            Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
        })
        .await?;
    let mut brackets = Vec::new();
    for (id, ember_id, site, tournament, name, stored) in running {
        let Some(grant) =
            organizer_grant(&state, &Site::Configured(site.clone()), &ember_id).await?
        else {
            continue;
        };
        brackets.push(json!({
            "id": id,
            "site": site,
            "tournament": tournament,
            "name": name,
            "settings": settings(&stored),
            "access_token": grant.access,
            "expires_at": grant.expires_at,
        }));
    }
    Ok(ok(&json!({ "brackets": brackets })))
}

/// Stored settings, written by `start` as a JSON object.
fn settings(stored: &str) -> Value {
    serde_json::from_str(stored).unwrap_or_else(|_| json!({}))
}
