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
mod pages;
mod provider;

use axum::{
    Form, Router,
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, post},
};
use ember_protocol::{
    challenge::Method,
    discord,
    encoding::b64u,
    sign_in::{Connection, SignInStarted},
    sites,
};
use rusqlite::Transaction;
use serde::Deserialize;
use serde_json::json;

pub use self::provider::{Profile, is_user_id};
use self::{
    pages::Pages,
    provider::{OAuth, SignedIn},
};
use crate::{
    AppState, auth,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, json, ok},
    linking::{
        Site,
        sign_ins::{self, Question},
    },
    rate::Slot,
    routes::sessions::{self, Nothing},
    util::random,
};

/// Sign-ins one Ember ID may start per hour, on each site.
const STARTS_PER_HOUR: usize = 10;

/// Adds Discord's sign-in routes and the configured sites' (`/v1/sites/...`).
pub fn routes(app: Router<AppState>) -> Router<AppState> {
    app.route(
        discord::START_PATH,
        post(|state, headers, body| start(state, headers, body, Site::Discord))
            .delete(|state, headers, body| cancel(state, headers, body, Site::Discord)),
    )
    .route(
        discord::CALLBACK_PATH,
        get(|state, answer| callback(state, answer, Site::Discord))
            .post(|state, answer| decide(state, answer, Site::Discord)),
    )
    .route(
        discord::ACCOUNT_PATH,
        get(|state, headers| account(state, headers, Site::Discord))
            .delete(|state, headers, body| remove(state, headers, body, Site::Discord)),
    )
    .route(
        "/v1/sites/{site}/start",
        post(
            |state: State<AppState>, Path(id): Path<String>, headers, body| async move {
                start(state.clone(), headers, body, configured(&state, &id)?).await
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
            |state: State<AppState>, Path(id): Path<String>, answer| async move {
                match configured(&state, &id) {
                    Ok(site) => callback(state, answer, site).await,
                    Err(failure) => failure.into_response(),
                }
            },
        )
        .post(
            |state: State<AppState>, Path(id): Path<String>, answer| async move {
                match configured(&state, &id) {
                    Ok(site) => decide(state, answer, site).await,
                    Err(failure) => failure.into_response(),
                }
            },
        ),
    )
    .route(
        "/v1/sites/{site}",
        get(
            |state: State<AppState>, Path(id): Path<String>, headers| async move {
                let site = held(&state, &id).await?;
                account(state, headers, site).await
            },
        )
        .delete(
            |state: State<AppState>, Path(id): Path<String>, headers, body| async move {
                let site = held(&state, &id).await?;
                remove(state, headers, body, site).await
            },
        ),
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

/// The site `id` names for reading and disconnecting accounts: a configured
/// one, or one taken out of the configuration while accounts are still
/// connected on it, so its players can still see and disconnect them (as
/// Discord's can with its sign-in off). Otherwise `not_found`.
async fn held(state: &AppState, id: &str) -> Result<Site> {
    if let Ok(site) = configured(state, id) {
        return Ok(site);
    }
    if !sites::is_site_id(id) || id == "discord" {
        return Err(ApiFailure::not_found());
    }
    let site = Site::Configured(id.to_owned());
    let lookup = site.clone();
    if state.db.read(move |tx| lookup.has_accounts(tx)).await? {
        Ok(site)
    } else {
        Err(ApiFailure::not_found())
    }
}

/// Whether `site`'s sign-in is offered. Reading and disconnecting a
/// connected account need no offer, so a player can always withdraw one.
pub fn offered(state: &AppState, site: &Site) -> bool {
    OAuth::of(state, site).is_some()
}

fn enabled(state: &AppState, site: &Site) -> Result<OAuth> {
    OAuth::of(state, site).ok_or_else(ApiFailure::not_found)
}

fn state_hash(state: &AppState, profile: &Profile, text: &str) -> [u8; 32] {
    state
        .keys
        .keyed_hash(&profile.domain("sign-in"), text.as_bytes())
}

/// The key of a sign-in waiting on the page's question: a secret of its own,
/// made when the question is asked and shown only on that page. The sign-in's
/// state was handed to the Ember that started it, so it does not answer the
/// question.
fn confirmation_hash(state: &AppState, profile: &Profile, text: &str) -> [u8; 32] {
    state
        .keys
        .keyed_hash(&profile.domain("move"), text.as_bytes())
}

fn redirect_uri(state: &AppState, profile: &Profile) -> String {
    format!("{}{}", state.config.origin, profile.callback)
}

/// Runs the session player's proof of `action` on `method` and `path`.
async fn proven<T: Send + 'static>(
    state: &AppState,
    headers: &HeaderMap,
    body: &Body<PROOF_BODY>,
    target: (ember_protocol::challenge::Action, Method, String),
    work: impl FnOnce(&Transaction<'_>, &Ctx, &str) -> Result<T> + Send + 'static,
) -> Result<T> {
    let player = auth::player(state, headers).await?;
    let (request, _): (_, Nothing) = sessions::proven(&body.0)?;
    sessions::as_proven(state, player, request, target, work).await
}

/// `POST <start>`: where to send the player's browser.
async fn start(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
    site: Site,
) -> Result<Response> {
    let oauth = enabled(&state, &site)?;
    let profile = Profile::of(&state.config, &site);
    let player = auth::player(&state, &headers).await?;
    state
        .rate
        .check(
            &format!("{}:{}", profile.domain("start"), player.ember_id),
            STARTS_PER_HOUR,
            3600,
            state.now(),
        )
        .map_err(ApiFailure::rate_limited)?;
    if let Some(limits) = oauth.limits
        && let Some(cap) = budget::refusing(&state, site.id(), limits).await?
    {
        return Err(ApiFailure::sign_in_busy(cap.retry_after(state.now())));
    }
    let secret = b64u(&random::<32>());
    let hash = state_hash(&state, &profile, &secret);
    let expires_at = state.now() + profile.sign_in_secs;
    let authorize_url = oauth
        .authorize_url(&redirect_uri(&state, &profile), &secret)
        .ok_or_else(ApiFailure::unavailable)?;
    let (request, _): (_, Nothing) = sessions::proven(&body.0)?;
    let target = (profile.connect, Method::Post, profile.start.clone());
    let (rate, per_hour, starts) = (
        state.rate.clone(),
        oauth.limits.map(|limits| limits.starts_per_hour),
        profile.domain("starts"),
    );
    let stored = site.clone();
    let started = sessions::as_proven(&state, player, request, target, move |tx, ctx, ember_id| {
        // A start counts against the bridge-wide limit only once its proof
        // holds, so requests with bad proofs cannot hold everyone's sign-in,
        // and only if it commits: the slot is given back otherwise.
        let slot = match per_hour.map(|per_hour| rate.take(&starts, per_hour, 3600, ctx.now)) {
            Some(Err(retry_after)) => return Ok(Err(retry_after)),
            Some(Ok(slot)) => Some(slot),
            None => None,
        };
        sign_ins::expire(tx, ctx.now)?;
        // Only the latest sign-in can connect an account.
        sign_ins::end(tx, &stored, ember_id)?;
        sign_ins::insert(tx, &stored, &hash, ember_id, expires_at)?;
        Ok(Ok(slot))
    })
    .await?;
    match started {
        Ok(slot) => slot.into_iter().for_each(Slot::keep),
        Err(retry_after) => {
            budget::refused(&state, site.id(), budget::Cap::StartsPerHour);
            return Err(ApiFailure::sign_in_busy(Some(retry_after)));
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
    let profile = Profile::of(&state.config, &site);
    let target = (profile.cancel, Method::Delete, profile.start);
    proven(&state, &headers, &body, target, move |tx, _, ember_id| {
        sign_ins::end(tx, &site, ember_id)
    })
    .await?;
    Ok(ok(&json!({})))
}

/// `GET <account>`: the session player's connected account.
async fn account(
    State(state): State<AppState>,
    headers: HeaderMap,
    site: Site,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
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
    site: Site,
) -> Result<Response> {
    let profile = Profile::of(&state.config, &site);
    let target = (profile.remove, Method::Delete, profile.account);
    proven(&state, &headers, &body, target, move |tx, ctx, ember_id| {
        site.disconnect(tx, ctx, ember_id)
    })
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
    let profile = Profile::of(&state.config, &site);
    // Only a sign-in Ember is waiting for reaches the site, and only for the
    // first answer that claims it; another answer for it is turned away. The
    // claimer takes it, whatever the site answered, in the same write that
    // stores the account, so a sign-in a disconnect ended meanwhile connects
    // nothing.
    let secret = answer.state.unwrap_or_default();
    let hash = state_hash(&state, &profile, &secret);
    let now = state.now();
    let claiming = site.clone();
    let claimed = state
        .db
        .write(move |tx| sign_ins::claim(tx, &claiming, &hash, now))
        .await;
    if !matches!(claimed, Ok(true)) {
        return pages.not_waiting();
    }
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
    let user = match oauth.user(&redirect_uri(&state, &profile), &code).await {
        Some(SignedIn::User(user)) => user,
        Some(SignedIn::Unusable(username)) => {
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
    let confirmation_key = confirmation_hash(&state, &profile, &confirmation);
    let username_key = oauth.username_key(&user.username);
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
            site.connect(
                tx,
                &ctx,
                &ember_id,
                &user.id,
                &user.username,
                username_key.as_deref(),
            )?;
            Ok(Stored::Connected(ember_id))
        })
        .await;
    match stored {
        Ok(Stored::Connected(ember_id)) => {
            state.committed();
            pages.connected(&shown, &ember_id)
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
    /// Connected, or left where it was: the Ember ID asked about and the account.
    Answered(String, String),
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
    let profile = Profile::of(&state.config, &site);
    let moves = match answer.choice.as_str() {
        "move" | "connect" => true,
        "keep" | "refuse" => false,
        _ => return pages.not_waiting(),
    };
    let refused = answer.choice == "refuse";
    let hash = confirmation_hash(&state, &profile, &answer.confirmation);
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
            }
            Ok(Some(Decided::Answered(
                question.ember_id,
                question.username,
            )))
        })
        .await;
    let (ember_id, username) = match decided {
        Ok(Some(Decided::Answered(ember_id, username))) => (ember_id, username),
        Ok(Some(Decided::Changed)) => {
            state.committed();
            return pages.changed();
        }
        Ok(None) => return pages.not_waiting(),
        Err(_) => return pages.not_saved(),
    };
    state.committed();
    if moves {
        pages.connected(&username, &ember_id)
    } else if refused {
        pages.refused(&username)
    } else {
        pages.kept(&username)
    }
}
