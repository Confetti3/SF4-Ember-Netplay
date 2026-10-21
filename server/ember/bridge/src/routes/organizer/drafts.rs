//! Drafts (design 4a): a title, then the site's event, a start time,
//! settings and links, created on the site with the organizer's grant and
//! marked ready to run.
use super::*;

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
pub(super) struct Link {
    pub(super) label: String,
    pub(super) url: String,
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
                // An event picked from the site's list is not Ember's to
                // delete there, whatever this draft created before.
                let taken = tx.execute(
                    "UPDATE organizer_events SET site = ?1, tournament = ?2, name = ?3,
                         created_on_site = created_on_site AND site IS ?1 AND tournament IS ?2
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
    let organizer = caller.ember_id().to_owned();
    let ctx = Ctx::of(&state);
    state
        .db
        .write(move |tx| caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, &path))
        .await?;
    state.committed();
    if let Some(limits) = oauth.limits
        && !spend(&state, site.id(), &organizer, 1, limits).await?
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
    let organizer = caller.ember_id().to_owned();
    let ctx = Ctx::of(&state);
    state
        .db
        .write(move |tx| caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, &path))
        .await?;
    state.committed();
    if let Some(limits) = oauth.limits
        && !spend(&state, site.id(), &organizer, 1, limits).await?
    {
        return Err(ApiFailure::sign_in_busy(None));
    }
    oauth
        .start(&grant.access, &tournament)
        .await
        .map_err(|refused| site_refused(&state, site.id(), "to start the bracket", refused))?;
    Ok(ok(&json!({})))
}
