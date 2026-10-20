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

mod drafts;
mod sets;

use drafts::Link;
pub use drafts::{change, create, draft, ready, start_bracket};
use sets::held_sets;
pub use sets::{adjudicate, is_desk_action, mark, public, put_sets, sets};

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

/// The name a configured site is shown by, or its ID.
pub(crate) fn site_name(state: &AppState, id: &str) -> String {
    state
        .config
        .sign_in_site(id)
        .map_or_else(|| id.to_owned(), |site| site.display_name.clone())
}
