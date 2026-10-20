//! Signing in with another site to connect its account to an Ember ID:
//! Discord (on when the configuration has `discord`), and each sign-in site
//! the configuration defines (`config::sites`), each with its client secret
//! in the integration secrets. One flow serves all of them; what differs is
//! in `provider`, and for a configured site it is only data.
//!
//! A player's Ember asks to start a sign-in with the site's `connect` proof
//! and opens the returned page in the browser; the site sends the browser
//! back to the callback with a code, which the bridge exchanges for the
//! player's user ID and username on the site. The bridge keeps those two, so
//! platforms that find players by that account get the Ember ID. Nothing
//! else from the site is stored, and the access token is dropped.
//!
//! An organizer sign-in (`/v1/sites/<id>/organize`, for a site whose
//! definition has `organizer`) asks for the site's organizer scopes and does
//! all of the above, and also keeps the tokens, sealed, as the account's
//! grant (`linking::grants`) once the account is the Ember ID's. The bridge
//! refreshes them when they are used (`organizer_grant`).
//!
//! A platform's link approved by a sign-in follows the account as
//! `linking::site` describes: it ends when the account moves or is
//! disconnected, and a link the player removes is not made again until a
//! newer sign-in.
//!
//! A sign-in connects nothing by itself unless the account is already this
//! Ember ID's: the browser page names the Ember ID it would connect to and
//! asks first, so an authorize link someone else started cannot tie the
//! person who opens it to their Ember ID. For an account connected to
//! another Ember ID the page also says what moving it ends. The answer holds
//! only for the owner the page named: if the account was connected or moved
//! elsewhere meanwhile, it changes nothing.
//! Starting a sign-in ends the Ember ID's earlier ones on that site, and the
//! player can end the one in flight (`cancel`), so only the latest can
//! connect. The pages the browser shows are in `pages`, and the sign-in rows
//! are kept by `linking::sign_ins`. A site whose definition limits its
//! requests has its starts limited across the bridge too, and its exchanges
//! counted in `budget`.
mod budget;
pub(crate) mod desk;
mod pages;
mod provider;

pub use self::{
    budget::spend,
    provider::{OAuth, Tournament},
};

use axum::{
    Form, Router,
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode},
    response::Response,
    routing::{get, post},
};
use ember_protocol::{
    EmberId, discord,
    encoding::b64u,
    sign_in::{Connection, SIGN_IN_SECS, SignInStarted},
    sites,
};
use rusqlite::Transaction;
use serde::Deserialize;
use serde_json::json;

use self::{
    pages::Pages,
    provider::{SignedIn, Step, Tokens},
};
use crate::{
    AppState, auth,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json, ok},
    linking::{
        Site,
        grants::{self, Grant},
        sign_ins::{self, Question},
        site_has_accounts,
    },
    rate::Slot,
    routes::sessions::{Call, Nothing},
    util::random,
};

/// Sign-ins one Ember ID may start per hour, on each site.
const STARTS_PER_HOUR: usize = 10;

/// How long before it lapses an organizer's access token is refreshed, and
/// how long one lasts when the site does not say.
const REFRESH_MARGIN: u64 = 24 * 3600;

/// Adds Discord's sign-in routes and the configured sites' (`/v1/sites/...`).
pub fn routes(app: Router<AppState>) -> Router<AppState> {
    app.route(
        discord::START_PATH,
        post(|state, headers, body| start(state, headers, body, Site::Discord, false))
            .delete(|state, headers, body| cancel(state, headers, body, Site::Discord)),
    )
    .route(
        discord::CALLBACK_PATH,
        get(|state, answer| callback(state, answer, Site::Discord))
            .post(|state, answer| decide(state, answer, Site::Discord)),
    )
    .route(
        discord::ACCOUNT_PATH,
        get(|state, headers| account(state, headers, None))
            .delete(|state, headers, body| remove(state, headers, body, None)),
    )
    .route(
        "/v1/sites/{site}/start",
        post(
            |state: State<AppState>, Path(id): Path<String>, headers, body| async move {
                start(
                    state.clone(),
                    headers,
                    body,
                    configured(&state, &id)?,
                    false,
                )
                .await
            },
        )
        .delete(
            |state: State<AppState>, Path(id): Path<String>, headers, body| async move {
                cancel(state.clone(), headers, body, configured(&state, &id)?).await
            },
        ),
    )
    .route(
        "/v1/sites/{site}/callback",
        get(
            |state: State<AppState>,
             Path(id): Path<String>,
             headers: HeaderMap,
             Query(answer): Query<Answer>| async move {
                let site = match configured(&state, &id) {
                    Ok(site) => site,
                    Err(_) => return pages::unknown(),
                };
                // The web desk's sign-ins come back here too, marked.
                match answer
                    .state
                    .clone()
                    .as_deref()
                    .and_then(|s| s.strip_prefix(desk::WEB))
                {
                    Some(secret) => desk::callback(state.0, &headers, answer, site, secret).await,
                    None => callback(state, Query(answer), site).await,
                }
            },
        )
        .post(
            |state: State<AppState>, Path(id): Path<String>, answer| async move {
                match configured(&state, &id) {
                    Ok(site) => decide(state, answer, site).await,
                    Err(_) => pages::unknown(),
                }
            },
        ),
    )
    .route(
        "/v1/sites/{site}/organize",
        post(
            |state: State<AppState>, Path(id): Path<String>, headers, body| async move {
                start(state.clone(), headers, body, configured(&state, &id)?, true).await
            },
        )
        .delete(
            |state: State<AppState>, Path(id): Path<String>, headers, body| async move {
                withdraw(state.clone(), headers, body, configured(&state, &id)?).await
            },
        )
        .get(
            |state: State<AppState>, Path(id): Path<String>, headers| async move {
                organizer(state.clone(), headers, configured(&state, &id)?).await
            },
        ),
    )
    .route(
        "/v1/sites/{site}",
        get(|state, Path(id): Path<String>, headers| account(state, headers, Some(id))).delete(
            |state, Path(id): Path<String>, headers, body| remove(state, headers, body, Some(id)),
        ),
    )
    .route("/web/sign-in/{site}", get(desk::start))
    .route("/web/session", get(desk::session))
    .route("/web/sign-out", post(desk::sign_out))
    .route("/web/accounts", get(desk::accounts))
    .route("/web/organize/{site}", post(desk::organize))
    .route(
        desk::WEB_SESSIONS_PATH,
        get(desk::web_sessions).delete(desk::end_web_sessions),
    )
    .route("/v1/web-sessions/seen", post(desk::web_sessions_seen))
    .route(desk::HANDOFF_PATH, post(desk::handoff))
    .route("/desk/h/{token}", get(desk::handoff_link))
    .route(desk::APPROVE_PATH, post(desk::approve))
    .route(desk::DEVICE_PATH, get(desk::device_info))
    .route(
        "/web/device",
        post(desk::device).get(desk::device_signed_in),
    )
}

/// The configured site `id` names, or `not_found`.
fn configured(state: &AppState, id: &str) -> Result<Site> {
    state
        .config
        .sign_in_site(id)
        .map(|_| Site::Configured(id.to_owned()))
        .ok_or_else(ApiFailure::not_found)
}

/// The site an account route names, for `player` to read or disconnect
/// their account on: Discord (`None`), a configured site, or one taken out
/// of the configuration while accounts are still connected on it, so its
/// players can still see and disconnect them (as Discord's can with its
/// sign-in off). Otherwise `not_found`. It takes the authenticated player
/// so that only one learns whether a removed site still has accounts.
async fn held(state: &AppState, _player: &auth::Player, id: Option<String>) -> Result<Site> {
    let Some(id) = id else {
        return Ok(Site::Discord);
    };
    if let Ok(site) = configured(state, &id) {
        return Ok(site);
    }
    if !sites::is_site_id(&id) || id == "discord" {
        return Err(ApiFailure::not_found());
    }
    let lookup = id.clone();
    if state
        .db
        .read(move |tx| site_has_accounts(tx, &lookup))
        .await?
    {
        Ok(Site::Configured(id))
    } else {
        Err(ApiFailure::not_found())
    }
}

/// Whether `site`'s sign-in is offered: the configuration has it and the
/// integration secrets hold its client secret. Reading and disconnecting a
/// connected account need no offer, so a player can always withdraw one.
pub fn offered(state: &AppState, site: &Site) -> bool {
    match site {
        Site::Discord => {
            state.config.discord.is_some() && state.integrations.discord_client_secret.is_some()
        }
        Site::Configured(id) => {
            state.config.sign_in_site(id).is_some()
                && state.integrations.site_client_secrets.contains_key(id)
        }
    }
}

fn enabled(state: &AppState, site: &Site) -> Result<OAuth> {
    OAuth::of(state, site).ok_or_else(ApiFailure::not_found)
}

fn state_hash(state: &AppState, site: &Site, text: &str) -> [u8; 32] {
    state
        .keys
        .keyed_hash(&site.domain("sign-in"), text.as_bytes())
}

/// The key of a sign-in waiting on the page's question: a secret of its own,
/// made when the question is asked and shown only on that page. The sign-in's
/// state was handed to the Ember that started it, so it does not answer the
/// question.
fn confirmation_hash(state: &AppState, site: &Site, text: &str) -> [u8; 32] {
    state.keys.keyed_hash(&site.domain("move"), text.as_bytes())
}

fn redirect_uri(state: &AppState, site: &Site) -> String {
    format!("{}{}", state.config.origin, site.callback_path())
}

/// `POST <start>`, or `POST <organize>` for an organizer sign-in: where to
/// send the player's browser.
async fn start(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
    site: Site,
    organizer: bool,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    begin(&state, site, organizer, Starter::Player(player, body)).await
}

/// Who starts a sign-in: the session player, with the proof that
/// authorizes it, or the organizer signed in to the web desk, whose CSRF
/// token the caller has checked (`desk::organize`).
pub(super) enum Starter {
    Player(auth::Player, Body<PROOF_BODY>),
    Desk(String),
}

/// Starts `starter`'s sign-in on `site`, an organizer sign-in when
/// `organizer`: the site's authorize page and when the sign-in lapses.
pub(super) async fn begin(
    state: &AppState,
    site: Site,
    organizer: bool,
    starter: Starter,
) -> Result<Response> {
    let oauth = if organizer {
        OAuth::organizer(state, &site).ok_or_else(ApiFailure::not_found)?
    } else {
        enabled(state, &site)?
    };
    let starting = match &starter {
        Starter::Player(player, _) => player.ember_id.as_str().to_owned(),
        Starter::Desk(ember_id) => ember_id.clone(),
    };
    state
        .rate
        .check(
            &format!("{}:{starting}", site.domain("start")),
            STARTS_PER_HOUR,
            3600,
            state.now(),
        )
        .map_err(ApiFailure::rate_limited)?;
    let secret = b64u(&random::<32>());
    let hash = state_hash(state, &site, &secret);
    let expires_at = state.now() + SIGN_IN_SECS;
    let authorize_url = oauth
        .authorize_url(&redirect_uri(state, &site), &secret)
        .ok_or_else(ApiFailure::unavailable)?;
    let (rate, limits, starts) = (state.rate.clone(), oauth.limits, site.domain("starts"));
    let stored = site.clone();
    let target = site.target(if organizer {
        Step::Organize
    } else {
        Step::Connect
    });
    let work = move |tx: &Transaction<'_>, ctx: &Ctx, ember_id: &str| {
        // Once the proof holds, in the same write: the site's request
        // budget must have room for the exchange, and the start takes a
        // bridge-wide slot, given back unless the start commits, so requests
        // with bad proofs cannot hold everyone's sign-in.
        let mut slot = None;
        if let Some(limits) = limits {
            if let Some(cap) = budget::full(tx, stored.id(), ctx.now, limits)? {
                return Ok(Err((cap, cap.retry_after(ctx.now))));
            }
            match rate.take(&starts, limits.starts_per_hour, 3600, ctx.now) {
                Ok(taken) => slot = Some(taken),
                Err(retry_after) => {
                    return Ok(Err((budget::Cap::StartsPerHour, Some(retry_after))));
                }
            }
        }
        sign_ins::expire(tx, ctx.now)?;
        // Only the latest sign-in can connect an account.
        sign_ins::end(tx, &stored, ember_id)?;
        sign_ins::insert(tx, &stored, &hash, ember_id, expires_at, organizer)?;
        Ok(Ok(slot))
    };
    let started = match starter {
        Starter::Player(player, body) => {
            Call::from(target)
                .run(
                    state,
                    &player,
                    &body,
                    Nothing::check,
                    move |tx, ctx, id: &EmberId, _: Nothing| work(tx, ctx, id.as_str()),
                )
                .await?
        }
        Starter::Desk(ember_id) => {
            let ctx = Ctx::of(state);
            let started = state.db.write(move |tx| work(tx, &ctx, &ember_id)).await?;
            state.committed();
            started
        }
    };
    match started {
        Ok(slot) => slot.into_iter().for_each(Slot::keep),
        Err((cap, retry_after)) => {
            budget::refused(state, site.id(), cap);
            return Err(ApiFailure::sign_in_busy(retry_after));
        }
    }
    Ok(json(
        StatusCode::CREATED,
        &SignInStarted {
            authorize_url,
            expires_at,
        },
    ))
}

/// `DELETE <start>`: ends the player's sign-ins in flight, so a page left
/// open in the browser connects nothing. Needs no sign-in offer, like
/// disconnecting.
async fn cancel(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
    site: Site,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    Call::from(site.target(Step::Cancel))
        .run(
            &state,
            &player,
            &body,
            Nothing::check,
            move |tx, _, ember_id, _: Nothing| sign_ins::end(tx, &site, ember_id.as_str()),
        )
        .await?;
    Ok(ok(&json!({})))
}

/// `GET <account>`: the session player's connected account.
async fn account(
    State(state): State<AppState>,
    headers: HeaderMap,
    site: Option<String>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let site = held(&state, &player, site).await?;
    let account = state
        .db
        .read(move |tx| site.account(tx, player.ember_id.as_str()))
        .await?;
    Ok(ok(&Connection { account }))
}

/// `DELETE <account>`: disconnects the session player's account.
async fn remove(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
    site: Option<String>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let site = held(&state, &player, site).await?;
    Call::from(site.target(Step::Remove))
        .run(
            &state,
            &player,
            &body,
            Nothing::check,
            move |tx, ctx, ember_id, _: Nothing| site.disconnect(tx, ctx, ember_id.as_str()),
        )
        .await?;
    Ok(ok(&Connection { account: None }))
}

#[derive(Deserialize)]
pub struct Answer {
    #[serde(default)]
    code: Option<String>,
    #[serde(default)]
    state: Option<String>,
    #[serde(default)]
    error: Option<String>,
}

/// What the callback did with a sign-in the site confirmed.
enum Stored {
    Connected(String),
    /// The account is not `to`'s yet: connected to `from`, or to no Ember
    /// ID. `replaced` is the account `to` has now, which connecting this one
    /// disconnects. The player decides.
    Asking {
        from: Option<String>,
        to: String,
        replaced: Option<String>,
        confirmation: String,
    },
    Gone,
}

/// `GET <callback>`: the site's answer, in the player's browser. The page
/// says what happened, or asks before connecting an account that is not this
/// Ember ID's yet; the player goes back to the site that sent them.
async fn callback(
    State(state): State<AppState>,
    Query(answer): Query<Answer>,
    site: Site,
) -> Response {
    let pages = Pages::of(&state.config, &site);
    let Ok(oauth) = enabled(&state, &site) else {
        return pages.off();
    };
    // Only a sign-in Ember is waiting for reaches the site, and only for the
    // first answer that claims it; another answer for it is turned away. The
    // claimer takes it, whatever the site answered, in the same write that
    // stores the account, so a sign-in a disconnect ended meanwhile connects
    // nothing.
    let secret = answer.state.unwrap_or_default();
    let hash = state_hash(&state, &site, &secret);
    let now = state.now();
    let claiming = site.clone();
    let claimed = state
        .db
        .write(move |tx| sign_ins::claim(tx, &claiming, &hash, now))
        .await;
    let Ok(Some(organizer)) = claimed else {
        return pages.not_waiting();
    };
    let oauth = match (organizer, OAuth::organizer(&state, &site)) {
        (false, _) => oauth,
        (true, Some(organizer)) => organizer,
        (true, None) => {
            end_claimed(&state, &site, hash, now).await;
            return pages.off();
        }
    };
    let code = match (answer.error.as_deref(), answer.code) {
        (None, Some(code)) if !code.is_empty() && code.len() <= 256 => code,
        _ => {
            end_claimed(&state, &site, hash, now).await;
            return pages.not_connected();
        }
    };
    if let Some(limits) = oauth.limits
        && !matches!(budget::spend(&state, site.id(), limits).await, Ok(true))
    {
        end_claimed(&state, &site, hash, now).await;
        return pages.busy();
    }
    let (user, tokens) = match oauth.user(&redirect_uri(&state, &site), &code).await {
        Some((SignedIn::User(user), tokens)) => (user, tokens),
        Some((SignedIn::Unusable(username), _)) => {
            end_claimed(&state, &site, hash, now).await;
            return pages.unusable(&username);
        }
        None => {
            end_claimed(&state, &site, hash, now).await;
            return pages.no_answer();
        }
    };
    let shown = user.username.clone();
    let confirmation = b64u(&random::<32>());
    let confirmation_key = confirmation_hash(&state, &site, &confirmation);
    let username_key = oauth.username_key(&user.username);
    let grant = organizer.then(|| grant(tokens, &oauth, now).seal(&state.keys));
    let ctx = Ctx::of(&state);
    let stored = state
        .db
        .write(move |tx| {
            let Some(ember_id) = sign_ins::claimed(tx, &site, &hash, ctx.now)? else {
                return Ok(Stored::Gone);
            };
            let owner = site.owner(tx, &user.id)?;
            if owner.as_deref() != Some(ember_id.as_str()) {
                let replaced = site.account(tx, &ember_id)?.map(|account| account.username);
                // The owner the question names is kept, so the answer holds
                // only while it still is.
                let question = Question {
                    ember_id,
                    user_id: user.id,
                    username: user.username,
                    owner,
                    grant,
                };
                sign_ins::ask(tx, &site, &hash, &confirmation_key, &question)?;
                return Ok(Stored::Asking {
                    from: question.owner,
                    to: question.ember_id,
                    replaced,
                    confirmation,
                });
            }
            sign_ins::take(tx, &site, &hash, ctx.now)?;
            // A player sign-in of the account this Ember ID already holds
            // keeps its organizer grant, which the fresh row would drop.
            let kept = match grant {
                Some(_) => None,
                None => grants::held(tx, site.id(), &ember_id)?
                    .filter(|(held, _)| *held == user.id)
                    .map(|(_, sealed)| sealed),
            };
            site.connect(
                tx,
                &ctx,
                &ember_id,
                &user.id,
                &user.username,
                username_key.as_deref(),
            )?;
            if let Some(grant) = grant.or(kept) {
                grants::save(tx, site.id(), &user.id, &grant, ctx.now)?;
            }
            Ok(Stored::Connected(ember_id))
        })
        .await;
    match stored {
        Ok(Stored::Connected(ember_id)) => {
            state.committed();
            pages.connected(&shown, &ember_id, organizer)
        }
        Ok(Stored::Asking {
            from,
            to,
            replaced,
            confirmation,
        }) => {
            state.committed();
            let replaced = replaced.as_deref();
            match from {
                Some(from) => pages.moving(&confirmation, &shown, &from, &to, replaced),
                None => pages.connecting(&confirmation, &shown, &to, replaced),
            }
        }
        Ok(Stored::Gone) => pages.not_waiting(),
        Err(_) => pages.not_saved(),
    }
}

/// Ends the claimed sign-in `hash` names, in a write of its own, when the
/// site's answer connects nothing. If the write fails, the sign-in lapses.
async fn end_claimed(state: &AppState, site: &Site, hash: [u8; 32], now: u64) {
    let site = site.clone();
    let _ = state
        .db
        .write(move |tx| sign_ins::take(tx, &site, &hash, now).map(drop))
        .await;
}

#[derive(Deserialize)]
pub struct MoveAnswer {
    confirmation: String,
    choice: String,
}

/// What the player's answer did.
enum Decided {
    /// Connected, or left where it was: the Ember ID asked about, the account,
    /// and whether it was an organizer sign-in.
    Answered(String, String, bool),
    /// The account was connected or moved elsewhere after the page asked.
    Changed,
}

/// `POST <callback>`: the player's answer to connecting or moving an
/// account, from the page `callback` showed. Either way the sign-in is done.
async fn decide(
    State(state): State<AppState>,
    Form(answer): Form<MoveAnswer>,
    site: Site,
) -> Response {
    let pages = Pages::of(&state.config, &site);
    let Ok(oauth) = enabled(&state, &site) else {
        return pages.off();
    };
    let moves = match answer.choice.as_str() {
        "move" | "connect" => true,
        "keep" | "refuse" => false,
        _ => return pages.not_waiting(),
    };
    let refused = answer.choice == "refuse";
    let hash = confirmation_hash(&state, &site, &answer.confirmation);
    let ctx = Ctx::of(&state);
    let decided = state
        .db
        .write(move |tx| {
            let Some(question) = sign_ins::take_question(tx, &site, &hash, ctx.now)? else {
                return Ok(None);
            };
            if moves {
                if site.owner(tx, &question.user_id)? != question.owner {
                    return Ok(Some(Decided::Changed));
                }
                site.connect(
                    tx,
                    &ctx,
                    &question.ember_id,
                    &question.user_id,
                    &question.username,
                    oauth.username_key(&question.username).as_deref(),
                )?;
                if let Some(grant) = &question.grant {
                    grants::save(tx, site.id(), &question.user_id, grant, ctx.now)?;
                }
            }
            let organizer = question.grant.is_some();
            Ok(Some(Decided::Answered(
                question.ember_id,
                question.username,
                organizer,
            )))
        })
        .await;
    let (ember_id, username, organizer) = match decided {
        Ok(Some(Decided::Answered(ember_id, username, organizer))) => {
            (ember_id, username, organizer)
        }
        Ok(Some(Decided::Changed)) => {
            state.committed();
            return pages.changed();
        }
        Ok(None) => return pages.not_waiting(),
        Err(_) => return pages.not_saved(),
    };
    state.committed();
    if moves {
        pages.connected(&username, &ember_id, organizer)
    } else if refused {
        pages.refused(&username)
    } else {
        pages.kept(&username)
    }
}

/// The grant an organizer sign-in keeps from `tokens`.
fn grant(tokens: Tokens, oauth: &OAuth, now: u64) -> Grant {
    Grant {
        access: tokens.access.to_string(),
        refresh: tokens.refresh.map(|refresh| refresh.to_string()),
        scope: oauth.scope().to_owned(),
        expires_at: now + tokens.expires_in.unwrap_or(REFRESH_MARGIN),
    }
}

/// `DELETE <organize>`: drops the session player's organizer grant on the
/// site. The account stays connected.
async fn withdraw(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
    site: Site,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let target = site.target(Step::Withdraw);
    Call::from(target)
        .run(
            &state,
            &player,
            &body,
            Nothing::check,
            move |tx, _, ember_id, _: Nothing| {
                sign_ins::end(tx, &site, ember_id.as_str())?;
                grants::remove(tx, site.id(), ember_id.as_str())
            },
        )
        .await?;
    Ok(ok(&json!({ "organizer": null })))
}

/// `GET <organize>`: the session player's organizer grant on the site, if
/// they hold one that still works: its scope and when its access token
/// lapses. Reading it refreshes it when due.
async fn organizer(
    State(state): State<AppState>,
    headers: HeaderMap,
    site: Site,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let grant = organizer_grant(&state, &site, player.ember_id.as_str()).await?;
    Ok(ok(&json!({
        "organizer": grant.map(|grant| json!({ "scope": grant.scope, "expires_at": grant.expires_at })),
    })))
}

/// The Ember ID's organizer grant on `site`, refreshed first when its
/// access token lapses within `REFRESH_MARGIN`. A refresh the site refuses
/// keeps the grant while its access token still works; after that there is
/// none until the organizer signs in again.
// ponytail: two refreshes at once may both spend the refresh token; one
// per grant at a time if a site rotates refresh tokens strictly.
pub async fn organizer_grant(
    state: &AppState,
    site: &Site,
    ember_id: &str,
) -> Result<Option<Grant>> {
    let (id, ember_id) = (site.id().to_owned(), ember_id.to_owned());
    let Some((user_id, sealed)) = state
        .db
        .read(move |tx| grants::held(tx, &id, &ember_id))
        .await?
    else {
        return Ok(None);
    };
    let Some(mut grant) = Grant::open(&state.keys, &sealed) else {
        return Ok(None);
    };
    let now = state.now();
    if grant.expires_at > now + REFRESH_MARGIN {
        return Ok(Some(grant));
    }
    let Some(oauth) = OAuth::organizer(state, site) else {
        return Ok(None);
    };
    // A refresh spends from a limited site's budget as a sign-in does.
    let spent = match (&grant.refresh, oauth.limits) {
        (None, _) => false,
        (Some(_), Some(limits)) => budget::spend(state, site.id(), limits).await?,
        (Some(_), None) => true,
    };
    let tokens = match grant.refresh.as_deref() {
        Some(refresh) if spent => oauth.refresh(&redirect_uri(state, site), refresh).await,
        _ => None,
    };
    let Some(tokens) = tokens else {
        return Ok((grant.expires_at > now).then_some(grant));
    };
    // A site that keeps the refresh token answers without one.
    let kept = grant.refresh.take();
    let mut fresh = self::grant(tokens, &oauth, now);
    fresh.refresh = fresh.refresh.take().or(kept);
    let (id, sealed) = (site.id().to_owned(), fresh.seal(&state.keys));
    state
        .db
        .write(move |tx| grants::save(tx, &id, &user_id, &sealed, now))
        .await?;
    Ok(Some(fresh))
}
