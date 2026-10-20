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
use ember_protocol::{
    api::ErrorCode,
    challenge::{Action, Method, ProvenRequest},
    encoding::is_prefixed_id,
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use serde_json::{Value, json};

use crate::{
    AppState, auth,
    ctx::Ctx,
    delivery::calls::{self, Called},
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, bearer, json, ok},
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
    let ember_id = reader(&state, &headers).await?;
    let site = Site::Configured(listing.site);
    let tournaments = listed(&state, &site, &ember_id).await?;
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
/// that authorizes it, or the organizer signed in to the web desk, whose
/// change carries the session's CSRF token instead (`sign_in::desk`).
pub(crate) enum Caller {
    Player {
        player: auth::Player,
        request: ProvenRequest,
    },
    /// The web desk's organizer, and whether the session was opened from
    /// the game.
    Desk { ember_id: String, from_game: bool },
}

impl Caller {
    /// The command in `body`, and who sent it.
    pub(crate) async fn of<C: DeserializeOwned>(
        state: &AppState,
        headers: &HeaderMap,
        body: &Body<PROOF_BODY>,
    ) -> Result<(Self, C)> {
        if bearer(headers).is_none()
            && let Some(desk) = auth::desk(state, headers).await?
        {
            if !desk.csrf_ok(headers) {
                return Err(ApiFailure::forbidden());
            }
            let command = ember_protocol::json::parse_as(&body.0, PROOF_BODY)?;
            let caller = Self::Desk {
                ember_id: desk.ember_id,
                from_game: desk.from_game,
            };
            return Ok((caller, command));
        }
        let player = auth::player(state, headers).await?;
        let (request, command) = sessions::proven(&body.0)?;
        Ok((Self::Player { player, request }, command))
    }

    /// Refuses a web desk signed in with a site alone for a change that
    /// needs the game: one a stolen site account must not make (the
    /// event's webhook, who may read it or sign up, running or ending it).
    /// A desk opened from the game makes it.
    pub(crate) fn require_game(&self) -> Result<()> {
        match self {
            Self::Player { .. }
            | Self::Desk {
                from_game: true, ..
            } => Ok(()),
            Self::Desk { .. } => Err(ApiFailure::new(
                ErrorCode::Forbidden,
                "Open the desk from Ember (Ember ID › Tournament desk).",
            )
            .detail("reason", "needs_game_proof")),
        }
    }

    pub(crate) fn ember_id(&self) -> &str {
        match self {
            Self::Player { player, .. } => player.ember_id.as_str(),
            Self::Desk { ember_id, .. } => ember_id,
        }
    }

    /// Consumes a player's proof for `action` on `method` and `path` in
    /// `tx`; the Ember ID it names, or the desk organizer's.
    pub(crate) fn prove(
        &self,
        tx: &Transaction<'_>,
        ctx: &Ctx,
        action: Action,
        method: Method,
        path: &str,
    ) -> Result<String> {
        let Self::Player { player, request } = self else {
            return Ok(self.ember_id().to_owned());
        };
        let target = Target {
            action,
            method,
            path,
        };
        let (ember_id, _) = sessions::consume_proof(
            tx,
            &ctx.config,
            ctx.now,
            request,
            target,
            Some(&player.ember_id),
        )?;
        Ok(ember_id.as_str().to_owned())
    }
}

/// The organizer a read is for: the session player, or the one signed in
/// to the web desk.
async fn reader(state: &AppState, headers: &HeaderMap) -> Result<String> {
    if bearer(headers).is_none()
        && let Some(desk) = auth::desk(state, headers).await?
    {
        return Ok(desk.ember_id);
    }
    Ok(auth::player(state, headers)
        .await?
        .ember_id
        .as_str()
        .to_owned())
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
    caller.require_game()?;
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
    let ember_id = reader(&state, &headers).await?;
    let now = state.now();
    let keys = state.keys.clone();
    let events = state
        .db
        .read(move |tx| {
            let mut query = tx.prepare(
                "SELECT id, title, status, starts_at, site, tournament, name, settings, links,
                     created_at, discord_webhook, post_results, sign_ups,
                     (SELECT COUNT(*) FROM event_sign_ups s WHERE s.event_id = organizer_events.id),
                     public, created_on_site
                 FROM organizer_events
                 WHERE ember_id = ?1 AND ended_at IS NULL ORDER BY created_at DESC, id",
            )?;
            let rows = query.query_map([&ember_id], |row| {
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
                    // Never the URL itself: it posts to the channel.
                    "discord_webhook": row
                        .get::<_, Option<Vec<u8>>>(10)?
                        .and_then(|sealed| keys.open(&sealed))
                        .and_then(|url| String::from_utf8(url.to_vec()).ok())
                        .map(|url| calls::masked(&url)),
                    "post_results": row.get::<_, bool>(11)?,
                    "sign_ups": row.get::<_, bool>(12)?,
                    "signed_up": row.get::<_, u64>(13)?,
                    "public": row.get::<_, bool>(14)?,
                    "created_on_site": row.get::<_, bool>(15)?,
                }))
            })?;
            Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
        })
        .await?;
    let events: Vec<Value> = events
        .into_iter()
        .map(|mut event| {
            let starts = event["site"].as_str().is_some_and(|site| {
                OAuth::organizer(&state, &Site::Configured(site.to_owned()))
                    .is_some_and(|oauth| oauth.starts())
            });
            event["starts_on_site"] = json!(starts);
            event
        })
        .collect();
    Ok(ok(&json!({ "events": events })))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Stop {
    /// Also deletes the site's event, one Ember created there.
    #[serde(default)]
    on_site: bool,
}

/// `DELETE /v1/organizer/events/{id}`: deletes one of the session player's
/// drafts, or stops one of their events; with `on_site`, first deletes the
/// event Ember created for it on its site, and changes nothing here when
/// the site does not. Takes a `site.remove` proof for this path.
pub async fn stop(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, command): (_, Stop) = Caller::of(&state, &headers, &body).await?;
    caller.require_game()?;
    let path = format!("{EVENTS_PATH}/{id}");
    let ctx = Ctx::of(&state);
    if command.on_site {
        let (reading, owner) = (id.clone(), caller.ember_id().to_owned());
        let created: Option<(String, String)> = state
            .db
            .read(move |tx| {
                Ok(tx
                    .query_row(
                        "SELECT site, tournament FROM organizer_events WHERE id = ?1
                             AND ember_id = ?2 AND ended_at IS NULL AND created_on_site = 1",
                        params![reading, owner],
                        |row| Ok((row.get(0)?, row.get(1)?)),
                    )
                    .optional()?)
            })
            .await?;
        let (site, tournament) = created
            .ok_or_else(|| ApiFailure::invalid("Ember deletes only the events it created."))?;
        let site = Site::Configured(site);
        let oauth = OAuth::organizer(&state, &site)
            .filter(OAuth::deletes_created)
            .ok_or_else(|| ApiFailure::invalid("This site's events are deleted on the site."))?;
        let grant = organizer_grant(&state, &site, caller.ember_id())
            .await?
            .ok_or_else(ApiFailure::not_found)?;
        // As with creating: the proof is spent before the site is asked.
        let owner = caller.ember_id().to_owned();
        let proving = ctx.clone();
        state
            .db
            .write(move |tx| caller.prove(tx, &proving, Action::SiteRemove, Method::Delete, &path))
            .await?;
        state.committed();
        if let Some(limits) = oauth.limits
            && !spend(&state, site.id(), limits).await?
        {
            return Err(ApiFailure::sign_in_busy(None));
        }
        oauth
            .delete_created(&grant.access, &tournament)
            .await
            .map_err(|refused| site_refused(&state, site.id(), "to delete the event", refused))?;
        let stopped = state.db.write(move |tx| end(tx, &ctx, &id, &owner)).await?;
        state.committed();
        return if stopped {
            Ok(ok(&json!({})))
        } else {
            Err(ApiFailure::not_found())
        };
    }
    let stopped = state
        .db
        .write(move |tx| {
            let ember_id = caller.prove(tx, &ctx, Action::SiteRemove, Method::Delete, &path)?;
            end(tx, &ctx, &id, &ember_id)
        })
        .await?;
    state.committed();
    if !stopped {
        return Err(ApiFailure::not_found());
    }
    Ok(ok(&json!({})))
}

/// Deletes `ember_id`'s draft `id`, or ends their event `id`, and cancels
/// its matches on the desk connection that have not ended, so its players
/// are free for other sets; whether there was one.
fn end(tx: &Transaction<'_>, ctx: &Ctx, id: &str, ember_id: &str) -> Result<bool> {
    let now = ctx.now;
    let event: Option<(Option<String>, Option<String>)> = tx
        .query_row(
            "SELECT site, tournament FROM organizer_events
             WHERE id = ?1 AND ember_id = ?2 AND ended_at IS NULL",
            params![id, ember_id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?;
    if let (Some((Some(site), Some(tournament))), Some(desk)) = (&event, &ctx.config.desk) {
        // The connector names a set's match `<site>:<tournament>:<set>:<attempt>`.
        let prefix = format!("{site}:{tournament}:");
        let ids: Vec<String> = tx
            .prepare(
                "SELECT id FROM matches WHERE connection_id = ?1
                 AND substr(external_match_id, 1, length(?2)) = ?2",
            )?
            .query_map(params![desk.connection, prefix], |row| row.get(0))?
            .collect::<rusqlite::Result<_>>()?;
        for match_id in ids {
            if let Some(found) = crate::routes::ledger::load(tx, &match_id)?
                && !found.state.is_terminal()
            {
                crate::routes::matches::cancel_and_release(tx, ctx, &found, "event_stopped")?;
            }
        }
    }
    let deleted = tx.execute(
        "DELETE FROM organizer_events WHERE id = ?1 AND ember_id = ?2 AND status = 'draft'",
        params![id, ember_id],
    )?;
    Ok(deleted == 1
        || tx.execute(
            "UPDATE organizer_events SET ended_at = ?1
             WHERE id = ?2 AND ember_id = ?3 AND ended_at IS NULL",
            params![now, id, ember_id],
        )? == 1)
}

/// What a site's refusal of `what` becomes: its own words, or unavailable
/// when it gave none.
fn site_refused(state: &AppState, site: &str, what: &str, refused: Option<String>) -> ApiFailure {
    let Some(said) = refused else {
        return ApiFailure::unavailable();
    };
    eprintln!("organizer: {site} refused {what}: {said}");
    ApiFailure::new(
        ErrorCode::InvalidRequest,
        format!("{} refused {what}: {said}", site_name(state, site)),
    )
    .detail("reason", "site_refused")
    .detail("site_said", said)
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
    #[serde(default, deserialize_with = "given")]
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
    /// Whether anyone may read the event and its sets
    /// (`GET /v1/public/events/{id}`).
    #[serde(default)]
    public: Option<bool>,
    /// The Discord channel webhook the desk posts calls to
    /// (`delivery::calls`), or null to take it away.
    #[serde(default, deserialize_with = "given")]
    discord_webhook: Option<Value>,
    /// Whether a finished set's score is posted there too.
    #[serde(default)]
    post_results: Option<bool>,
    /// Whether players may sign up through Ember (`routes::sign_ups`),
    /// for an event created on a site that takes participants.
    #[serde(default)]
    sign_ups: Option<bool>,
}

/// A field that was given, null included; serde's own `Option` reads a
/// null as not given.
fn given<'de, D: serde::Deserializer<'de>>(
    field: D,
) -> std::result::Result<Option<Value>, D::Error> {
    Value::deserialize(field).map(Some)
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
    if change.discord_webhook.is_some() || change.public.is_some() || change.sign_ups.is_some() {
        caller.require_game()?;
    }
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
    let webhook = match change.discord_webhook {
        None => None,
        Some(Value::Null) => Some(None),
        Some(url) => {
            let url = url
                .as_str()
                .and_then(|url| calls::check_webhook(url, state.config.allow_loopback_http))
                .ok_or_else(|| {
                    ApiFailure::invalid(
                        "discord_webhook is a Discord channel webhook URL \
                         (https://discord.com/api/webhooks/...), or null.",
                    )
                })?;
            Some(Some(state.keys.seal(url.as_bytes())))
        }
    };
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
            if let Some(sign_ups) = change.sign_ups {
                let site: Option<String> = tx.query_row(
                    "SELECT site FROM organizer_events WHERE id = ?1",
                    [&id],
                    |row| row.get(0),
                )?;
                let takes = site
                    .and_then(|site| ctx.config.sign_in_site(&site))
                    .and_then(|site| site.organizer.as_ref())
                    .is_some_and(|organizer| organizer.participants.is_some());
                if sign_ups && !takes {
                    return Ok(Err(Some(
                        "Players sign up through Ember only for an event on a site that takes them.",
                    )));
                }
                tx.execute(
                    "UPDATE organizer_events SET sign_ups = ?1 WHERE id = ?2",
                    params![sign_ups, id],
                )?;
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
            if let Some(public) = change.public {
                tx.execute(
                    "UPDATE organizer_events SET public = ?1 WHERE id = ?2",
                    params![public, id],
                )?;
            }
            if let Some(webhook) = webhook {
                tx.execute(
                    "UPDATE organizer_events SET discord_webhook = ?1 WHERE id = ?2",
                    params![webhook, id],
                )?;
            }
            if let Some(post_results) = change.post_results {
                tx.execute(
                    "UPDATE organizer_events SET post_results = ?1 WHERE id = ?2",
                    params![post_results, id],
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
    caller.require_game()?;
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
        .map_err(|refused| site_refused(&state, site.id(), "the settings", refused))?;
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
                "UPDATE organizer_events SET site = ?1, tournament = ?2, name = title, links = ?3,
                     created_on_site = 1
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
    caller.require_game()?;
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

/// `POST /v1/organizer/events/{id}/start`: starts the bracket of a ready
/// event on its site, with the session player's organizer grant, when the
/// site starts brackets from Ember. Takes a `site.connect` proof for this
/// path.
pub async fn start_bracket(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, sessions::Nothing {}) = Caller::of(&state, &headers, &body).await?;
    caller.require_game()?;
    let (reading, owner) = (id.clone(), caller.ember_id().to_owned());
    let ready: Option<(String, String)> = state
        .db
        .read(move |tx| {
            Ok(tx
                .query_row(
                    "SELECT site, tournament FROM organizer_events WHERE id = ?1
                         AND ember_id = ?2 AND status = 'ready' AND ended_at IS NULL",
                    params![reading, owner],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?)
        })
        .await?;
    let (site, tournament) = ready.ok_or_else(ApiFailure::not_found)?;
    let site = Site::Configured(site);
    let oauth = OAuth::organizer(&state, &site)
        .filter(OAuth::starts)
        .ok_or_else(|| ApiFailure::invalid("This site's brackets are started on the site."))?;
    let grant = organizer_grant(&state, &site, caller.ember_id())
        .await?
        .ok_or_else(ApiFailure::not_found)?;
    let path = format!("{EVENTS_PATH}/{id}/start");
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
    oauth
        .start(&grant.access, &tournament)
        .await
        .map_err(|refused| site_refused(&state, site.id(), "to start the bracket", refused))?;
    Ok(ok(&json!({})))
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

/// A title as kept: 1 to 60 characters once the spaces around it are
/// dropped, on one line. Challonge refuses a longer tournament name, and the
/// title is the name Ember creates it under.
fn title_of(title: &str) -> Result<String> {
    let title = title.trim();
    if (1..=60).contains(&title.chars().count()) && !title.chars().any(char::is_control) {
        Ok(title.to_owned())
    } else {
        Err(ApiFailure::invalid(
            "A title is 1 to 60 characters on one line.",
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
        let mut settings = settings(&stored);
        let event = id.clone();
        let held = state.db.read(move |tx| held_sets(tx, &event)).await?;
        if !held.is_empty() {
            if !settings["hold"].is_object() {
                settings["hold"] = json!({});
            }
            settings["hold"]["sets"] = json!(held);
        }
        brackets.push(json!({
            "id": id,
            "site": site,
            "tournament": tournament,
            "name": name,
            "settings": settings,
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

/// The largest set snapshot the connector sends: `MAX_SETS` sets of a few
/// hundred bytes each.
const SETS_BODY: usize = 512 * 1024;

/// The most sets a snapshot keeps (the connector's `snapshot::MAX_SETS`).
const MAX_SETS: usize = 1024;

#[derive(Deserialize)]
struct Snapshot {
    sets: Vec<Value>,
}

/// `PUT /v1/desk/events/{id}/sets`: the bracket connector's latest view of
/// a running event's sets, kept as sent until the next.
pub async fn put_sets(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<SETS_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let desk = state
        .config
        .desk
        .as_ref()
        .ok_or_else(ApiFailure::not_found)?;
    if service.provider_connection()? != desk.connection {
        return Err(ApiFailure::forbidden());
    }
    let snapshot: Snapshot = body.parse()?;
    if snapshot.sets.len() > MAX_SETS || snapshot.sets.iter().any(|set| !set.is_object()) {
        return Err(ApiFailure::invalid(
            "sets is a list of at most 1024 objects.",
        ));
    }
    let stored = serde_json::to_string(&snapshot.sets).map_err(|_| ApiFailure::unavailable())?;
    let now = state.now();
    let bridge_id = state.config.bridge_id.clone();
    let kept = state
        .db
        .write(move |tx| {
            let kept = tx.execute(
                "UPDATE organizer_events SET sets = ?1, sets_at = ?2
                 WHERE id = ?3 AND ended_at IS NULL AND status = 'ready'",
                params![stored, now, id],
            )?;
            if kept == 1 {
                post_news(tx, now, &bridge_id, &id, &stored)?;
            }
            Ok(kept)
        })
        .await?;
    if kept == 0 {
        return Err(ApiFailure::not_found());
    }
    state.committed();
    Ok(ok(&json!({})))
}

/// Calls each set whose Ember match the desk made since its last call, and
/// posts the score of each finished set the desk called, once per match
/// (`delivery::calls`).
fn post_news(
    tx: &Transaction<'_>,
    now: u64,
    bridge_id: &str,
    id: &str,
    stored: &str,
) -> Result<()> {
    let site: Option<String> = tx.query_row(
        "SELECT site FROM organizer_events WHERE id = ?1",
        [id],
        |row| row.get(0),
    )?;
    for set in desk_sets(tx, id, site.as_deref(), Some(stored), true)? {
        let (Some(set_id), Some(match_id)) = (set["id"].as_str(), set["match_id"].as_str()) else {
            continue;
        };
        let (called, reported): (Option<String>, Option<String>) = tx
            .query_row(
                "SELECT called_match, result_match FROM desk_marks
                 WHERE event_id = ?1 AND set_id = ?2",
                params![id, set_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?
            .unwrap_or_default();
        let called_set = Called {
            event_id: id,
            set: &set,
        };
        let state = set["desk_state"].as_str().unwrap_or_default();
        if called.as_deref() != Some(match_id)
            && matches!(state, "called" | "playing")
            && calls::call(tx, now, bridge_id, &called_set)?
        {
            tx.execute(
                "INSERT INTO desk_marks (event_id, set_id, called_match, called_at)
                 VALUES (?1, ?2, ?3, ?4)
                 ON CONFLICT (event_id, set_id)
                 DO UPDATE SET called_match = ?3, called_at = ?4",
                params![id, set_id, match_id, now],
            )?;
        }
        if called.as_deref() == Some(match_id)
            && reported.as_deref() != Some(match_id)
            && matches!(state, "reporting" | "done")
            && calls::result(tx, now, &called_set)?
        {
            tx.execute(
                "UPDATE desk_marks SET result_match = ?1 WHERE event_id = ?2 AND set_id = ?3",
                params![match_id, id, set_id],
            )?;
        }
    }
    Ok(())
}

/// `GET /v1/organizer/events/{id}/sets`: the organizer's event's sets, each
/// with its Ember match's score and state and the desk state they make.
pub async fn sets(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let ember_id = reader(&state, &headers).await?;
    let view = state
        .db
        .read(move |tx| {
            let found: Option<(Option<String>, Option<String>, Option<u64>)> = tx
                .query_row(
                    "SELECT site, sets, sets_at FROM organizer_events
                     WHERE id = ?1 AND ember_id = ?2",
                    params![id, ember_id],
                    |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
                )
                .optional()?;
            let Some((site, stored, at)) = found else {
                return Ok(None);
            };
            Ok(Some(json!({
                "sets": desk_sets(tx, &id, site.as_deref(), stored.as_deref(), true)?,
                "updated_at": at,
            })))
        })
        .await?;
    view.map(|view| ok(&view)).ok_or_else(ApiFailure::not_found)
}

/// `GET /v1/public/events/{id}`: an event its organizer made public and has
/// not stopped: its title, start, links and sets, without Ember IDs, and
/// whether it takes sign-ups now.
pub async fn public(State(state): State<AppState>, Path(id): Path<String>) -> Result<Response> {
    let view = state
        .db
        .read(move |tx| {
            type Row = (
                String,
                Option<u64>,
                Option<String>,
                String,
                Option<String>,
                Option<u64>,
                bool,
            );
            let found: Option<Row> = tx
                .query_row(
                    "SELECT title, starts_at, site, links, sets, sets_at,
                         sign_ups = 1 AND status = 'draft' AND tournament IS NOT NULL
                     FROM organizer_events
                     WHERE id = ?1 AND public = 1 AND ended_at IS NULL",
                    [&id],
                    |row| {
                        Ok((
                            row.get(0)?,
                            row.get(1)?,
                            row.get(2)?,
                            row.get(3)?,
                            row.get(4)?,
                            row.get(5)?,
                            row.get(6)?,
                        ))
                    },
                )
                .optional()?;
            let Some((title, starts_at, site, links, stored, at, sign_ups_open)) = found else {
                return Ok(None);
            };
            Ok(Some(json!({
                "title": title,
                "starts_at": starts_at,
                "site": site,
                "links": settings(&links),
                "sets": desk_sets(tx, &id, site.as_deref(), stored.as_deref(), false)?,
                "updated_at": at,
                "sign_ups_open": sign_ups_open,
            })))
        })
        .await?;
    view.map(|view| ok(&view)).ok_or_else(ApiFailure::not_found)
}

/// The sets an event's organizer holds from the desk.
fn held_sets(tx: &Transaction<'_>, event: &str) -> Result<Vec<String>> {
    Ok(tx
        .prepare("SELECT set_id FROM desk_marks WHERE event_id = ?1 AND held = 1 ORDER BY set_id")?
        .query_map([event], |row| row.get(0))?
        .collect::<rusqlite::Result<_>>()?)
}

/// Whether `path` is a desk action on one of an event's sets or matches,
/// which a `site.connect` proof authorizes (`sessions::route_matches`).
pub fn is_desk_action(path: &str) -> bool {
    let Some(rest) = path.strip_prefix("/v1/organizer/events/") else {
        return false;
    };
    let parts: Vec<&str> = rest.split('/').collect();
    match parts.as_slice() {
        [event, "sets", set] => is_prefixed_id(event, "oev") && is_set_id(set),
        [event, "matches", id, "adjudications"] => {
            is_prefixed_id(event, "oev") && is_prefixed_id(id, "emt")
        }
        _ => false,
    }
}

/// A set ID as the connector sends it: a site's ID, 1 to 64 letters,
/// digits, `-` or `_`.
fn is_set_id(id: &str) -> bool {
    (1..=64).contains(&id.len())
        && id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Mark {
    /// Holds the set (it is not played in Ember) or lets it go.
    #[serde(default)]
    held: Option<bool>,
    /// Calls the set again: the desk shows when.
    #[serde(default)]
    call: bool,
}

/// `POST /v1/organizer/events/{id}/sets/{set}`: holds or lets go a set of
/// the organizer's running event, or calls it again. Takes a `site.connect`
/// proof for this path.
pub async fn mark(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((id, set)): Path<(String, String)>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, mark): (_, Mark) = Caller::of(&state, &headers, &body).await?;
    if !is_set_id(&set) {
        return Err(ApiFailure::not_found());
    }
    let path = format!("{EVENTS_PATH}/{id}/sets/{set}");
    let ctx = Ctx::of(&state);
    let marked = state
        .db
        .write(move |tx| {
            let ember_id = caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, &path)?;
            if open_status(tx, &id, &ember_id)?.is_none() {
                return Ok(false);
            }
            tx.execute(
                "INSERT INTO desk_marks (event_id, set_id) VALUES (?1, ?2)
                 ON CONFLICT (event_id, set_id) DO NOTHING",
                params![id, set],
            )?;
            if let Some(held) = mark.held {
                tx.execute(
                    "UPDATE desk_marks SET held = ?1 WHERE event_id = ?2 AND set_id = ?3",
                    params![held, id, set],
                )?;
            }
            if mark.call {
                tx.execute(
                    "UPDATE desk_marks SET called_at = ?1 WHERE event_id = ?2 AND set_id = ?3",
                    params![ctx.now, id, set],
                )?;
                let (site, stored): (Option<String>, Option<String>) = tx.query_row(
                    "SELECT site, sets FROM organizer_events WHERE id = ?1",
                    [&id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )?;
                let found = desk_sets(tx, &id, site.as_deref(), stored.as_deref(), true)?
                    .into_iter()
                    .find(|shown| shown["id"] == json!(set))
                    .unwrap_or_else(|| json!({ "id": set }));
                let called = Called {
                    event_id: &id,
                    set: &found,
                };
                calls::call(tx, ctx.now, &ctx.config.bridge_id, &called)?;
            }
            Ok(true)
        })
        .await?;
    state.committed();
    if !marked {
        return Err(ApiFailure::not_found());
    }
    Ok(ok(&json!({})))
}

/// `POST /v1/organizer/events/{id}/matches/{match}/adjudications`: the
/// organizer decides or voids a game of an Ember match one of their events
/// created, as an organizer credential would (`matches::adjudicate`),
/// recorded as theirs. Takes a `site.connect` proof for this path.
pub async fn adjudicate(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((id, match_id)): Path<(String, String)>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, command): (_, crate::routes::matches::AdjudicationCommand) =
        Caller::of(&state, &headers, &body).await?;
    let desk = state
        .config
        .desk
        .as_ref()
        .ok_or_else(ApiFailure::not_found)?
        .connection
        .clone();
    let path = format!("{EVENTS_PATH}/{id}/matches/{match_id}/adjudications");
    let ctx = Ctx::of(&state);
    let result = state
        .db
        .write(move |tx| {
            let ember_id = caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, &path)?;
            // The match must be one this organizer's event made: on the
            // desk connection, and in the event's sets.
            let stored: Option<Option<String>> = tx
                .query_row(
                    "SELECT sets FROM organizer_events WHERE id = ?1 AND ember_id = ?2",
                    params![id, ember_id],
                    |row| row.get(0),
                )
                .optional()?;
            let sets: Vec<Value> = stored
                .flatten()
                .and_then(|stored| serde_json::from_str(&stored).ok())
                .unwrap_or_default();
            let on_desk: bool = tx.query_row(
                "SELECT EXISTS (SELECT 1 FROM matches WHERE id = ?1 AND connection_id = ?2)",
                params![match_id, desk],
                |row| row.get(0),
            )?;
            if !on_desk || !sets.iter().any(|set| set["match_id"] == json!(match_id)) {
                return Err(ApiFailure::not_found());
            }
            crate::routes::matches::adjudicate_for(
                tx,
                &ctx,
                &format!("desk:{ember_id}"),
                &match_id,
                &command,
            )
        })
        .await?;
    state.committed();
    let (status, body) = result;
    Ok(json(status, &body))
}

/// A kept snapshot's sets with what the bridge knows of each one's Ember
/// match: score, players (by their account on the event's site, else the
/// name the site shows for the entry; their Ember IDs when `with_ids`), the
/// round its metadata names when the snapshot has none, and the desk state.
/// Without `with_ids` no ID of the site's or Ember's is left in a set.
fn desk_sets(
    tx: &Transaction<'_>,
    event: &str,
    site: Option<&str>,
    stored: Option<&str>,
    with_ids: bool,
) -> Result<Vec<Value>> {
    let sets: Vec<Value> = stored
        .and_then(|stored| serde_json::from_str(stored).ok())
        .unwrap_or_default();
    let posted = calls::calls(tx, event)?;
    let marks: std::collections::HashMap<String, (bool, Option<u64>)> = tx
        .prepare("SELECT set_id, held, called_at FROM desk_marks WHERE event_id = ?1")?
        .query_map([event], |row| Ok((row.get(0)?, (row.get(1)?, row.get(2)?))))?
        .collect::<rusqlite::Result<_>>()?;
    sets.into_iter()
        .map(|mut set| {
            // Held from the desk shows held before the connector's next read.
            if let Some((held, called_at)) = set["id"].as_str().and_then(|id| marks.get(id)) {
                if *held {
                    set["held"] = json!(true);
                }
                set["called_at"] = json!(called_at);
            }
            if let Some(call) = set["id"].as_str().and_then(|id| posted.get(id)) {
                set["call"] = call.clone();
            }
            let found = match set["match_id"].as_str() {
                Some(match_id) => desk_match(tx, match_id, site)?,
                None => None,
            };
            // The site's names, each replaced by the player's account
            // name once the set has an Ember match.
            let mut players = match set["names"].take() {
                Value::Array(names) => names,
                _ => vec![Value::Null; 2],
            };
            if let Some(found) = &found {
                for key in ["round_label", "round", "phase"] {
                    if set[key].is_null() {
                        set[key] = found.metadata[key].clone();
                    }
                }
                set["score"] = json!(found.wins);
                // Drawn games are replayed and never scored, so the score
                // alone would hide them.
                set["draws"] = json!(
                    found
                        .games
                        .iter()
                        .filter(|game| game["state"] == "accepted" && game["outcome"] == "draw")
                        .count()
                );
                for (slot, name) in found.names.iter().enumerate() {
                    if let (Some(name), Some(player)) = (name, players.get_mut(slot)) {
                        *player = json!(name);
                    }
                }
                if with_ids {
                    set["ember_ids"] = json!(found.ember_ids);
                    // What a decision from the desk names as its
                    // `expected_revision`.
                    set["revision"] = json!(found.revision.to_string());
                    set["games"] = json!(found.games);
                }
            }
            if let Some(set) = set.as_object_mut() {
                set.remove("names");
            }
            set["players"] = json!(players);
            set["side"] = json!(side(&set));
            set["desk_state"] = json!(desk_state(&set, found.as_ref().map(|f| f.state.as_str())));
            if !with_ids && let Some(set) = set.as_object_mut() {
                set.remove("match_id");
                set.remove("entries");
            }
            Ok(set)
        })
        .collect()
}

struct DeskMatch {
    state: String,
    revision: u64,
    /// The games played or decided, oldest first: what the desk can void,
    /// and a game waiting for a result it can decide.
    games: Vec<Value>,
    metadata: Value,
    wins: [u8; 2],
    ember_ids: Vec<String>,
    names: Vec<Option<String>>,
}

fn desk_match(tx: &Transaction<'_>, id: &str, site: Option<&str>) -> Result<Option<DeskMatch>> {
    let found: Option<(String, String, i64, u64)> = tx
        .query_row(
            "SELECT state, metadata, assignment_generation, revision FROM matches WHERE id = ?1",
            [id],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
        )
        .optional()?;
    let Some((state, metadata, generation, revision)) = found else {
        return Ok(None);
    };
    let (wins, _) = crate::routes::ledger::scores(tx, id)?;
    let ember_ids = tx
        .prepare(
            "SELECT ember_id FROM match_participants
             WHERE match_id = ?1 AND assignment_generation = ?2 ORDER BY slot",
        )?
        .query_map(params![id, generation], |row| row.get(0))?
        .collect::<rusqlite::Result<Vec<String>>>()?;
    let games = tx
        .prepare(
            "SELECT id, outcome, state FROM attempts
             WHERE match_id = ?1 AND state IN ('accepted', 'review', 'permitted') ORDER BY seq",
        )?
        .query_map([id], |row| {
            Ok(json!({
                "attempt_id": row.get::<_, String>(0)?,
                "outcome": row.get::<_, Option<String>>(1)?,
                "state": row.get::<_, String>(2)?,
            }))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let mut names = Vec::new();
    for ember_id in &ember_ids {
        names.push(
            tx.query_row(
                "SELECT username FROM site_accounts WHERE site = ?1 AND ember_id = ?2",
                params![site, ember_id],
                |row| row.get(0),
            )
            .optional()?,
        );
    }
    Ok(Some(DeskMatch {
        state,
        revision,
        games,
        metadata: serde_json::from_str(&metadata).unwrap_or_else(|_| json!({})),
        wins,
        ember_ids,
        names,
    }))
}

/// Which part of the bracket a set is in: a grand final by its label, the
/// losers bracket below round 0, else winners.
fn side(set: &Value) -> &'static str {
    let label = set["round_label"].as_str().unwrap_or_default();
    if label.to_ascii_lowercase().contains("grand final") {
        "grand_final"
    } else if set["round"].as_i64().is_some_and(|round| round < 0) {
        "losers"
    } else {
        "winners"
    }
}

/// What the desk shows for a set: what the connector and the set's Ember
/// match say together.
fn desk_state(set: &Value, match_state: Option<&str>) -> &'static str {
    if set["held"] == json!(true) {
        return "held";
    }
    match (set["progress"].as_str(), match_state) {
        (Some("stuck"), _) => "needs_organizer",
        (Some("reported" | "closed"), _) => "done",
        (_, Some("needs_review")) => "needs_review",
        (_, Some("running" | "between_games" | "awaiting_reports")) => "playing",
        (_, Some("created" | "awaiting_players" | "provisioning" | "ready")) => "called",
        (_, Some("completed")) => "reporting",
        _ => match set["status"].as_str() {
            Some("decided") => "done",
            Some("pending") => "not_ready",
            _ => "waiting",
        },
    }
}

/// The name a configured site is shown by, or its ID.
pub(crate) fn site_name(state: &AppState, id: &str) -> String {
    state
        .config
        .sign_in_site(id)
        .map_or_else(|| id.to_owned(), |site| site.display_name.clone())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_sets_desk_state_and_side_come_from_the_connector_and_its_match() {
        let set = |fields: Value| fields;
        assert_eq!(
            desk_state(&set(json!({ "held": true })), Some("running")),
            "held"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "stuck" })), None),
            "needs_organizer"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "reported" })), Some("completed")),
            "done"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "live" })), Some("needs_review")),
            "needs_review"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "live" })), Some("between_games")),
            "playing"
        );
        assert_eq!(
            desk_state(
                &set(json!({ "progress": "live" })),
                Some("awaiting_players")
            ),
            "called"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "live" })), Some("completed")),
            "reporting"
        );
        assert_eq!(
            desk_state(&set(json!({ "progress": "live" })), Some("cancelled")),
            "waiting"
        );
        assert_eq!(
            desk_state(&set(json!({ "status": "pending" })), None),
            "not_ready"
        );
        assert_eq!(
            desk_state(&set(json!({ "status": "decided" })), None),
            "done"
        );
        assert_eq!(side(&json!({ "round": -2 })), "losers");
        assert_eq!(
            side(&json!({ "round": 5, "round_label": "Grand Final Reset" })),
            "grand_final"
        );
        assert_eq!(side(&json!({ "round": 3 })), "winners");
    }
}
