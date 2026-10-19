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
//!
//! An event can also start as a draft (design 4a): a title, then the site's
//! event (one the site lists for them, or one created there with their
//! grant), a start time, settings and links. Marked ready, it runs from its
//! start time. A draft makes no site calls of its own and is not handed out.
use axum::{
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::challenge::{Action, Method, ProvenRequest};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
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
pub const DRAFTS_PATH: &str = "/v1/organizer/drafts";

/// Events one organizer runs at once.
const RUNNING_PER_ORGANIZER: u64 = 8;

/// Drafts one organizer keeps at once.
const DRAFTS_PER_ORGANIZER: u64 = 20;

/// The links an event keeps: the bracket, a stream, a signup page and such.
const MAX_LINKS: usize = 12;

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

/// Who sends an organizer's command: the session player, with the proof
/// that authorizes it.
struct Caller {
    player: auth::Player,
    request: ProvenRequest,
}

impl Caller {
    /// The command in `body`, and the session player who sent it.
    async fn of<C: DeserializeOwned>(
        state: &AppState,
        headers: &HeaderMap,
        body: &Body<PROOF_BODY>,
    ) -> Result<(Self, C)> {
        let player = auth::player(state, headers).await?;
        let (request, command) = sessions::proven(&body.0)?;
        Ok((Self { player, request }, command))
    }

    fn ember_id(&self) -> &str {
        self.player.ember_id.as_str()
    }

    /// Consumes the proof for `action` on `method` and `path` in `tx`; the
    /// Ember ID it names.
    fn prove(
        &self,
        tx: &Transaction<'_>,
        ctx: &Ctx,
        action: Action,
        method: Method,
        path: &str,
    ) -> Result<String> {
        let target = Target {
            action,
            method,
            path,
        };
        let (ember_id, _) = sessions::consume_proof(
            tx,
            &ctx.config,
            ctx.now,
            &self.request,
            target,
            Some(&self.player.ember_id),
        )?;
        Ok(ember_id.as_str().to_owned())
    }
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
    let (caller, command): (_, Start) = Caller::of(&state, &headers, &body).await?;
    let settings = settings_of(command.settings)?;
    let site = Site::Configured(command.site.clone());
    let event = listed(&state, &site, caller.ember_id())
        .await?
        .into_iter()
        .find(|event| event.id == command.tournament)
        .ok_or_else(|| ApiFailure::invalid("The site lists no such event of yours."))?;
    let ctx = Ctx::of(&state);
    let id = new_id("oev");
    let started = id.clone();
    let done = state
        .db
        .write(move |tx| {
            let ember_id =
                caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, EVENTS_PATH)?;
            if running(tx, &ember_id)? >= RUNNING_PER_ORGANIZER {
                return Ok(Err("Stop one of your events first: 8 run at once."));
            }
            let inserted = tx.execute(
                "INSERT INTO organizer_events
                     (id, ember_id, title, status, site, tournament, name, settings, created_at)
                 VALUES (?1, ?2, ?5, 'ready', ?3, ?4, ?5, ?6, ?7)
                 ON CONFLICT DO NOTHING",
                params![
                    started,
                    ember_id,
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

/// `GET /v1/organizer/events`: the session player's drafts and events that
/// have not ended, newest first, each with its status: `draft`, `ready`
/// (waiting for its start time) or `running`.
pub async fn list(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let now = state.now();
    let events = state
        .db
        .read(move |tx| {
            let mut query = tx.prepare(
                "SELECT id, title, status, starts_at, site, tournament, name, settings, links,
                     created_at
                 FROM organizer_events
                 WHERE ember_id = ?1 AND ended_at IS NULL ORDER BY created_at DESC, id",
            )?;
            let rows = query.query_map([player.ember_id.as_str()], |row| {
                let starts_at: Option<u64> = row.get(3)?;
                let status = match row.get::<_, String>(2)?.as_str() {
                    "ready" if starts_at.is_none_or(|at| at <= now) => "running",
                    "ready" => "ready",
                    _ => "draft",
                };
                Ok(json!({
                    "id": row.get::<_, String>(0)?,
                    "title": row.get::<_, String>(1)?,
                    "status": status,
                    "starts_at": starts_at,
                    "site": row.get::<_, Option<String>>(4)?,
                    "tournament": row.get::<_, Option<String>>(5)?,
                    "name": row.get::<_, Option<String>>(6)?,
                    "settings": settings(&row.get::<_, String>(7)?),
                    "links": settings(&row.get::<_, String>(8)?),
                    "created_at": row.get::<_, u64>(9)?,
                }))
            })?;
            Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
        })
        .await?;
    Ok(ok(&json!({ "events": events })))
}

/// `DELETE /v1/organizer/events/{id}`: deletes one of the session player's
/// drafts, or stops one of their events. Takes a `site.remove` proof for
/// this path.
pub async fn stop(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, sessions::Nothing {}) = Caller::of(&state, &headers, &body).await?;
    let path = format!("{EVENTS_PATH}/{id}");
    let ctx = Ctx::of(&state);
    let stopped = state
        .db
        .write(move |tx| {
            let ember_id = caller.prove(tx, &ctx, Action::SiteRemove, Method::Delete, &path)?;
            let deleted = tx.execute(
                "DELETE FROM organizer_events WHERE id = ?1 AND ember_id = ?2 AND status = 'draft'",
                params![id, ember_id],
            )?;
            Ok(deleted == 1
                || tx.execute(
                    "UPDATE organizer_events SET ended_at = ?1
                     WHERE id = ?2 AND ember_id = ?3 AND ended_at IS NULL",
                    params![ctx.now, id, ember_id],
                )? == 1)
        })
        .await?;
    state.committed();
    if !stopped {
        return Err(ApiFailure::not_found());
    }
    Ok(ok(&json!({})))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Draft {
    title: String,
}

/// `POST /v1/organizer/drafts`: a new draft with only a title. Takes a
/// `site.connect` proof for this path.
pub async fn draft(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    if state.config.desk.is_none() {
        return Err(ApiFailure::not_found());
    }
    let (caller, command): (_, Draft) = Caller::of(&state, &headers, &body).await?;
    let title = title_of(&command.title)?;
    let ctx = Ctx::of(&state);
    let id = new_id("oev");
    let made = id.clone();
    let done = state
        .db
        .write(move |tx| {
            let ember_id =
                caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, DRAFTS_PATH)?;
            let drafts: u64 = tx.query_row(
                "SELECT COUNT(*) FROM organizer_events
                 WHERE ember_id = ?1 AND status = 'draft' AND ended_at IS NULL",
                [&ember_id],
                |row| row.get(0),
            )?;
            if drafts >= DRAFTS_PER_ORGANIZER {
                return Ok(Err("Delete one of your drafts first: you keep 20 at once."));
            }
            tx.execute(
                "INSERT INTO organizer_events (id, ember_id, title, status, settings, created_at)
                 VALUES (?1, ?2, ?3, 'draft', '{}', ?4)",
                params![made, ember_id, title, ctx.now],
            )?;
            Ok(Ok(()))
        })
        .await?;
    state.committed();
    done.map_err(ApiFailure::invalid)?;
    Ok(json(StatusCode::CREATED, &json!({ "id": id })))
}

/// A link kept with an event, shown as its label.
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Link {
    label: String,
    url: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Change {
    #[serde(default)]
    title: Option<String>,
    /// A time in seconds, or null to take the start time away.
    #[serde(default)]
    starts_at: Option<Value>,
    #[serde(default)]
    links: Option<Vec<Link>>,
    #[serde(default)]
    settings: Option<serde_json::Map<String, Value>>,
    /// The site's event a draft is for: one the site lists for them.
    #[serde(default)]
    site: Option<String>,
    #[serde(default)]
    tournament: Option<String>,
}

/// `POST /v1/organizer/events/{id}`: changes what is given of one of the
/// session player's events that has not ended. The site's event is given
/// only to a draft. Takes a `site.connect` proof for this path.
pub async fn change(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, change): (_, Change) = Caller::of(&state, &headers, &body).await?;
    let title = change.title.as_deref().map(title_of).transpose()?;
    let starts_at = match change.starts_at {
        None => None,
        Some(Value::Null) => Some(None),
        Some(at) => Some(Some(at.as_u64().ok_or_else(|| {
            ApiFailure::invalid("starts_at is a time in seconds, or null.")
        })?)),
    };
    let links = change.links.map(|links| links_of(&links)).transpose()?;
    let settings = change.settings.map(settings_of).transpose()?;
    let event = match (change.site, change.tournament) {
        (None, None) => None,
        (Some(site), Some(tournament)) => {
            let found = listed(&state, &Site::Configured(site.clone()), caller.ember_id())
                .await?
                .into_iter()
                .find(|event| event.id == tournament)
                .ok_or_else(|| ApiFailure::invalid("The site lists no such event of yours."))?;
            Some((site, found))
        }
        _ => return Err(ApiFailure::invalid("site and tournament go together.")),
    };
    let path = format!("{EVENTS_PATH}/{id}");
    let ctx = Ctx::of(&state);
    let done = state
        .db
        .write(move |tx| {
            let ember_id = caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, &path)?;
            let Some(status) = open_status(tx, &id, &ember_id)? else {
                return Ok(Err(None));
            };
            if let Some((site, event)) = event {
                if status != "draft" {
                    return Ok(Err(Some("Only a draft takes the site's event.")));
                }
                let taken = tx.execute(
                    "UPDATE organizer_events SET site = ?1, tournament = ?2, name = ?3
                     WHERE id = ?4
                       AND NOT EXISTS (SELECT 1 FROM organizer_events WHERE site = ?1
                           AND tournament = ?2 AND ended_at IS NULL AND id != ?4)",
                    params![site, event.id, event.name, id],
                )?;
                if taken == 0 {
                    return Ok(Err(Some("Another of Ember's events has that event.")));
                }
            }
            if let Some(title) = title {
                tx.execute(
                    "UPDATE organizer_events SET title = ?1 WHERE id = ?2",
                    params![title, id],
                )?;
            }
            if let Some(starts_at) = starts_at {
                tx.execute(
                    "UPDATE organizer_events SET starts_at = ?1 WHERE id = ?2",
                    params![starts_at, id],
                )?;
            }
            if let Some(links) = links {
                tx.execute(
                    "UPDATE organizer_events SET links = ?1 WHERE id = ?2",
                    params![links, id],
                )?;
            }
            if let Some(settings) = settings {
                tx.execute(
                    "UPDATE organizer_events SET settings = ?1 WHERE id = ?2",
                    params![settings, id],
                )?;
            }
            Ok(Ok(()))
        })
        .await?;
    state.committed();
    match done {
        Ok(()) => Ok(ok(&json!({}))),
        Err(None) => Err(ApiFailure::not_found()),
        Err(Some(refused)) => Err(ApiFailure::invalid(refused)),
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Create {
    site: String,
    #[serde(default)]
    fields: serde_json::Map<String, Value>,
}

/// `POST /v1/organizer/events/{id}/create`: creates a draft's event on the
/// site with the session player's organizer grant, named by the draft's
/// title and set as `fields` says, and gives it to the draft with the
/// event's page as its `Bracket` link. Takes a `site.connect` proof for
/// this path.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, command): (_, Create) = Caller::of(&state, &headers, &body).await?;
    let site = Site::Configured(command.site.clone());
    let oauth = OAuth::organizer(&state, &site).ok_or_else(ApiFailure::not_found)?;
    let (reading, owner) = (id.clone(), caller.ember_id().to_owned());
    let title: Option<String> = state
        .db
        .read(move |tx| {
            Ok(tx
                .query_row(
                    "SELECT title FROM organizer_events WHERE id = ?1 AND ember_id = ?2
                         AND status = 'draft' AND tournament IS NULL AND ended_at IS NULL",
                    params![reading, owner],
                    |row| row.get(0),
                )
                .optional()?)
        })
        .await?;
    let title = title.ok_or_else(ApiFailure::not_found)?;
    let creation = oauth
        .creation(&title, &command.fields)
        .ok_or_else(|| ApiFailure::invalid("This site's events are made on the site."))?
        .map_err(ApiFailure::invalid)?;
    let grant = organizer_grant(&state, &site, caller.ember_id())
        .await?
        .ok_or_else(ApiFailure::not_found)?;
    // The proof is spent before the site is asked, so a refused proof
    // creates nothing there.
    let path = format!("{EVENTS_PATH}/{id}/create");
    let ctx = Ctx::of(&state);
    state
        .db
        .write(move |tx| caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, &path))
        .await?;
    state.committed();
    if let Some(limits) = oauth.limits
        && !spend(&state, site.id(), limits).await?
    {
        return Err(ApiFailure::sign_in_busy(None));
    }
    let created = creation
        .send(&grant.access)
        .await
        .ok_or_else(ApiFailure::unavailable)?;
    let link = created.link.clone();
    let tournament = created.id.clone();
    let site_id = site.id().to_owned();
    state
        .db
        .write(move |tx| {
            let stored: String = tx.query_row(
                "SELECT links FROM organizer_events WHERE id = ?1",
                [&id],
                |row| row.get(0),
            )?;
            let mut links: Vec<Link> = serde_json::from_str(&stored).unwrap_or_default();
            if let Some(url) = link
                && links.len() < MAX_LINKS
            {
                links.insert(
                    0,
                    Link {
                        label: "Bracket".into(),
                        url,
                    },
                );
            }
            tx.execute(
                "UPDATE organizer_events SET site = ?1, tournament = ?2, name = title, links = ?3
                 WHERE id = ?4 AND status = 'draft' AND tournament IS NULL",
                params![site_id, tournament, json!(links).to_string(), id],
            )?;
            Ok(())
        })
        .await?;
    state.committed();
    Ok(json(
        StatusCode::CREATED,
        &json!({ "tournament": created.id, "link": created.link }),
    ))
}

/// `POST /v1/organizer/events/{id}/ready`: marks a draft that has its site's
/// event and a start time ready, so it runs from that time. Takes a
/// `site.connect` proof for this path.
pub async fn ready(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, sessions::Nothing {}) = Caller::of(&state, &headers, &body).await?;
    let path = format!("{EVENTS_PATH}/{id}/ready");
    let ctx = Ctx::of(&state);
    let done = state
        .db
        .write(move |tx| {
            let ember_id = caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, &path)?;
            let Some((tournament, starts_at)): Option<(Option<String>, Option<u64>)> = tx
                .query_row(
                    "SELECT tournament, starts_at FROM organizer_events
                     WHERE id = ?1 AND ember_id = ?2 AND status = 'draft' AND ended_at IS NULL",
                    params![id, ember_id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?
            else {
                return Ok(Err(None));
            };
            if tournament.is_none() || starts_at.is_none() {
                return Ok(Err(Some(
                    "A ready event needs its site's event and a start time.",
                )));
            }
            if running(tx, &ember_id)? >= RUNNING_PER_ORGANIZER {
                return Ok(Err(Some("Stop one of your events first: 8 run at once.")));
            }
            tx.execute(
                "UPDATE organizer_events SET status = 'ready' WHERE id = ?1",
                [&id],
            )?;
            Ok(Ok(()))
        })
        .await?;
    state.committed();
    match done {
        Ok(()) => Ok(ok(&json!({}))),
        Err(None) => Err(ApiFailure::not_found()),
        Err(Some(refused)) => Err(ApiFailure::invalid(refused)),
    }
}

/// The status of `ember_id`'s event `id` that has not ended, if any.
fn open_status(tx: &Transaction<'_>, id: &str, ember_id: &str) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            "SELECT status FROM organizer_events WHERE id = ?1 AND ember_id = ?2
                 AND ended_at IS NULL",
            params![id, ember_id],
            |row| row.get(0),
        )
        .optional()?)
}

/// How many of `ember_id`'s events are ready or running.
fn running(tx: &Transaction<'_>, ember_id: &str) -> Result<u64> {
    Ok(tx.query_row(
        "SELECT COUNT(*) FROM organizer_events
         WHERE ember_id = ?1 AND status = 'ready' AND ended_at IS NULL",
        [ember_id],
        |row| row.get(0),
    )?)
}

/// A title as kept: 1 to 100 characters once the spaces around it are
/// dropped, on one line.
fn title_of(title: &str) -> Result<String> {
    let title = title.trim();
    if (1..=100).contains(&title.chars().count()) && !title.chars().any(char::is_control) {
        Ok(title.to_owned())
    } else {
        Err(ApiFailure::invalid(
            "A title is 1 to 100 characters on one line.",
        ))
    }
}

/// Links as kept, as JSON: at most `MAX_LINKS`, each a label of 1 to 40
/// characters on one line and an https URL of at most 512 bytes with no
/// credentials.
fn links_of(links: &[Link]) -> Result<String> {
    let link_ok = |link: &Link| {
        let label = link.label.trim();
        (1..=40).contains(&label.chars().count())
            && !label.chars().any(char::is_control)
            && link.url.len() <= 512
            && url::Url::parse(&link.url).is_ok_and(|url| {
                url.scheme() == "https" && url.username().is_empty() && url.password().is_none()
            })
    };
    if links.len() > MAX_LINKS || !links.iter().all(link_ok) {
        return Err(ApiFailure::invalid(
            "Links are at most 12, each a label of 1 to 40 characters and an https URL.",
        ));
    }
    Ok(json!(links).to_string())
}

/// Settings as kept, as JSON: the keys the bracket connector reads, in at
/// most `MAX_SETTINGS` bytes.
fn settings_of(settings: serde_json::Map<String, Value>) -> Result<String> {
    let keys_ok = settings.keys().all(|key| SETTINGS.contains(&key.as_str()));
    let settings = Value::Object(settings).to_string();
    if !keys_ok || settings.len() > MAX_SETTINGS {
        return Err(ApiFailure::invalid(
            "settings take rounds, hold, active_from, active_until and mark_underway, \
             in at most 4096 bytes.",
        ));
    }
    Ok(settings)
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
    let now = state.now();
    let running: Vec<(String, String, String, String, String, String)> = state
        .db
        .read(move |tx| {
            let mut query = tx.prepare(
                "SELECT id, ember_id, site, tournament, name, settings FROM organizer_events
                 WHERE ended_at IS NULL AND status = 'ready'
                     AND (starts_at IS NULL OR starts_at <= ?1)
                 ORDER BY created_at, id",
            )?;
            let rows = query.query_map([now], |row| {
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

/// Stored settings or links, written as JSON by `settings_of` and
/// `links_of`.
fn settings(stored: &str) -> Value {
    serde_json::from_str(stored).unwrap_or_else(|_| json!({}))
}
