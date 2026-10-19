//! The web desk's sign-in (docs/design/TOURNAMENT_DESK.md, 4a): an organizer
//! signs in on the web with a site account their Ember ID already holds, and
//! the browser keeps a session cookie (`auth::DESK_COOKIE`) that the
//! organizer routes take in place of a player session, with the session's
//! CSRF token on every change.
//!
//! It comes back through the site's own callback with its state marked
//! `web.`, so the site's OAuth application needs no second redirect URI. The
//! browser that starts it must finish it: the state is also in a short
//! cookie, compared at the callback. Nothing from the site is kept, and an
//! account no Ember ID holds signs no one in: it is linked from the game.
use axum::{
    extract::{Path, State},
    http::{HeaderMap, HeaderValue, StatusCode, header},
    response::{IntoResponse, Redirect, Response},
};
use ember_protocol::{
    challenge::{Action, Method},
    encoding::b64u,
    sign_in::SIGN_IN_SECS,
};
use rusqlite::{OptionalExtension, params};
use serde_json::json;
use subtle::ConstantTimeEq;

use super::{
    Answer, OAuth, SignedIn, Starter, begin, budget, configured, offered, organizer_grant,
    redirect_uri,
};
use crate::{
    AppState,
    auth::{self, DESK_COOKIE, DESK_IDLE_SECS, DESK_SECS},
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, cookie, json as answer, ok, page},
    linking::Site,
    routes::{rooms::Address, sessions},
    util::{html, random},
};

/// Starts a web sign-in's state, after which comes its secret.
pub(super) const WEB: &str = "web.";

/// The cookie that ties a sign-in to the browser that started it.
const STATE_COOKIE: &str = "ember_desk_state";

/// Web sign-ins started on one site per hour, across the bridge, and from
/// one network address.
const STARTS_PER_HOUR: usize = 60;
const STARTS_PER_ADDRESS: usize = 10;

/// Where a signed-in browser goes.
const DESK: &str = "/desk";

fn state_hash(state: &AppState, site: &Site, secret: &str) -> [u8; 32] {
    state
        .keys
        .keyed_hash(&site.domain("desk-sign-in"), secret.as_bytes())
}

/// `GET /web/sign-in/{site}`: sends the browser to the site to sign in.
pub async fn start(
    State(state): State<AppState>,
    Path(id): Path<String>,
    Address(address): Address,
) -> Result<Response> {
    let site = configured(&state, &id)?;
    let oauth = OAuth::of(&state, &site)
        .filter(|_| offered(&state, &site))
        .ok_or_else(ApiFailure::not_found)?;
    let now = state.now();
    state
        .rate
        .check(
            &format!("desk-start:{address}"),
            STARTS_PER_ADDRESS,
            3600,
            now,
        )
        .map_err(ApiFailure::rate_limited)?;
    state
        .rate
        .check(&site.domain("desk-start"), STARTS_PER_HOUR, 3600, now)
        .map_err(ApiFailure::rate_limited)?;
    let secret = b64u(&random::<32>());
    let hash = state_hash(&state, &site, &secret);
    let authorize = oauth
        .authorize_url(&redirect_uri(&state, &site), &format!("{WEB}{secret}"))
        .ok_or_else(ApiFailure::unavailable)?;
    state
        .db
        .write(move |tx| {
            tx.execute("DELETE FROM desk_sign_ins WHERE expires_at <= ?1", [now])?;
            tx.execute(
                "INSERT INTO desk_sign_ins (site, state_hash, expires_at) VALUES (?1, ?2, ?3)",
                params![id, hash.as_slice(), now + SIGN_IN_SECS],
            )?;
            Ok(())
        })
        .await?;
    let mut response = Redirect::to(&authorize).into_response();
    set_cookie(&state, &mut response, STATE_COOKIE, &secret, SIGN_IN_SECS);
    Ok(response)
}

/// The site's answer to a web sign-in, whose state's secret is `secret`.
pub(super) async fn callback(
    state: AppState,
    headers: &HeaderMap,
    answer: Answer,
    site: Site,
    secret: &str,
) -> Response {
    let name = state
        .config
        .sign_in_site(site.id())
        .map(|site| site.display_name.clone())
        .unwrap_or_default();
    let same_browser = cookie(headers, STATE_COOKIE)
        .is_some_and(|started| bool::from(started.as_bytes().ct_eq(secret.as_bytes())));
    let hash = state_hash(&state, &site, secret);
    let now = state.now();
    let site_id = site.id().to_owned();
    let claimed = state
        .db
        .write(move |tx| {
            Ok(tx.execute(
                "DELETE FROM desk_sign_ins WHERE site = ?1 AND state_hash = ?2 AND expires_at > ?3",
                params![site_id, hash.as_slice(), now],
            )? == 1)
        })
        .await;
    if !same_browser || !matches!(claimed, Ok(true)) {
        return finished(
            "Sign-in expired",
            "This sign-in took longer than ten minutes, or started in another browser. \
             Sign in on the desk again.",
        );
    }
    let Some(oauth) = OAuth::of(&state, &site) else {
        return finished(
            "Sign-in is off",
            "This Ember service does not offer that sign-in.",
        );
    };
    let code = match (answer.error.as_deref(), answer.code) {
        (None, Some(code)) if !code.is_empty() && code.len() <= 256 => code,
        _ => {
            return finished(
                "Not signed in",
                "You did not sign in. Try again on the desk.",
            );
        }
    };
    if let Some(limits) = oauth.limits
        && !matches!(budget::spend(&state, site.id(), limits).await, Ok(true))
    {
        return finished(
            "Try again later",
            "Ember is asking this site too often right now. Try again in a while.",
        );
    }
    let Some((SignedIn::User(user), _)) = oauth.user(&redirect_uri(&state, &site), &code).await
    else {
        return finished(
            &format!("{name} did not answer"),
            &format!("{} did not confirm your account. Try again.", html(&name)),
        );
    };
    let keys = state.keys.clone();
    let agent = agent(headers);
    let session = state
        .db
        .write(move |tx| match site.owner(tx, &user.id)? {
            Some(ember_id) => {
                auth::start_desk_session(tx, &keys, &ember_id, site.id(), agent, false, now)
                    .map(Some)
            }
            None => Ok(None),
        })
        .await;
    match session {
        Ok(Some(token)) => {
            let mut response = Redirect::to(DESK).into_response();
            set_cookie(&state, &mut response, DESK_COOKIE, &token, DESK_SECS);
            set_cookie(&state, &mut response, STATE_COOKIE, "", 0);
            response
        }
        Ok(None) => {
            let name = html(&name);
            finished(
                "Link your account in Ember first",
                &format!(
                    "This {name} account is not linked to an Ember ID. In Ember, open Online \
                     play, Ember ID, and connect {name}; then sign in here again."
                ),
            )
        }
        Err(_) => finished(
            "Not signed in",
            "Ember could not keep the sign-in. Try again.",
        ),
    }
}

/// `GET /web/session`: who the desk's browser session is, with the CSRF
/// token its changes send in `X-Ember-CSRF`.
pub async fn session(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let desk = auth::desk(&state, &headers)
        .await?
        .ok_or_else(ApiFailure::unauthenticated)?;
    Ok(ok(&json!({
        "ember_id": desk.ember_id,
        "csrf": desk.csrf,
        "from_game": desk.from_game,
    })))
}

/// `GET /web/accounts`: the desk organizer's account on each sign-in site
/// the bridge offers; whether the site offers organizer sign-in and the
/// organizer grant they hold there; the settings a new event on the site
/// takes (`create`, none where events are made on the site itself, whose
/// pages are at `home`).
pub async fn accounts(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let desk = auth::desk(&state, &headers)
        .await?
        .ok_or_else(ApiFailure::unauthenticated)?;
    let mut sites = Vec::new();
    for defined in &state.config.sign_in_sites {
        let site = Site::Configured(defined.id.clone());
        if !offered(&state, &site) {
            continue;
        }
        let (reading, ember_id) = (site.clone(), desk.ember_id.clone());
        let account = state
            .db
            .read(move |tx| reading.account(tx, &ember_id))
            .await?;
        let organizer = OAuth::organizer(&state, &site).is_some();
        let grant = match (organizer, &account) {
            (true, Some(_)) => organizer_grant(&state, &site, &desk.ember_id).await?,
            _ => None,
        };
        let create = defined
            .organizer
            .as_ref()
            .and_then(|organizer| organizer.create.as_ref())
            .map(|create| {
                create
                    .fields
                    .iter()
                    .map(|(name, field)| (name.clone(), json!(field.values)))
                    .collect::<serde_json::Map<_, _>>()
            });
        let home = url::Url::parse(&defined.authorize_url)
            .map(|url| url.origin().ascii_serialization())
            .unwrap_or_default();
        sites.push(json!({
            "id": defined.id,
            "display_name": defined.display_name,
            "home": home,
            "account": account,
            "organizer_offered": organizer,
            "organizer": grant.map(|grant| json!({ "scope": grant.scope, "expires_at": grant.expires_at })),
            "create": create,
        }));
    }
    Ok(ok(&json!({ "sites": sites })))
}

/// `POST /web/organize/{site}`: starts the desk organizer's organizer
/// sign-in on the site, as `/v1/sites/{site}/organize` does for the game.
pub async fn organize(
    State(state): State<AppState>,
    Path(id): Path<String>,
    headers: HeaderMap,
) -> Result<Response> {
    let desk = auth::desk(&state, &headers)
        .await?
        .ok_or_else(ApiFailure::unauthenticated)?;
    if !desk.csrf_ok(&headers) {
        return Err(ApiFailure::forbidden());
    }
    let site = configured(&state, &id)?;
    begin(&state, site, true, Starter::Desk(desk.ember_id)).await
}

/// `POST /web/sign-out`: ends the desk's browser session.
pub async fn sign_out(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let desk = auth::desk(&state, &headers)
        .await?
        .ok_or_else(ApiFailure::unauthenticated)?;
    if !desk.csrf_ok(&headers) {
        return Err(ApiFailure::forbidden());
    }
    let token = cookie(&headers, DESK_COOKIE).unwrap_or_default();
    let token_hash = auth::hash(&state.keys, "desk", &token);
    state
        .db
        .write(move |tx| {
            tx.execute(
                "DELETE FROM desk_sessions WHERE token_hash = ?1",
                [token_hash.as_slice()],
            )?;
            Ok(())
        })
        .await?;
    let mut response = ok(&json!({}));
    set_cookie(&state, &mut response, DESK_COOKIE, "", 0);
    Ok(response)
}

/// A page that ends the sign-in, with a way back to the desk.
fn finished(title: &str, text: &str) -> Response {
    page(
        title,
        &format!(
            "<h1>{}</h1><p>{text}</p><p><a href=\"{DESK}\">Back to the desk</a></p>",
            html(title)
        ),
        false,
    )
}

/// Sets cookie `name` for the whole site, held from scripts, sent on a
/// top-level visit from the site's callback; Secure where the bridge's
/// origin is https. `max_age` 0 removes it.
fn set_cookie(state: &AppState, response: &mut Response, name: &str, value: &str, max_age: u64) {
    let secure = if state.config.origin.starts_with("https://") {
        "; Secure"
    } else {
        ""
    };
    if let Ok(cookie) = HeaderValue::from_str(&format!(
        "{name}={value}; Path=/; HttpOnly; SameSite=Lax; Max-Age={max_age}{secure}"
    )) {
        response.headers_mut().append(header::SET_COOKIE, cookie);
    }
}

/// The browser a sign-in came from, as the game lists it: a short name
/// read from its user agent, nothing that tells one browser from another.
fn agent(headers: &HeaderMap) -> &'static str {
    let agent = headers
        .get(header::USER_AGENT)
        .and_then(|value| value.to_str().ok())
        .unwrap_or_default();
    let has = |text: &str| agent.contains(text);
    let phone = has("Android") || has("iPhone") || has("iPad");
    match (
        phone,
        has("Firefox"),
        has("Edg/"),
        has("Chrome"),
        has("Safari"),
    ) {
        (true, true, ..) => "Firefox on a phone",
        (true, _, _, true, _) => "Chrome on a phone",
        (true, ..) => "Safari on a phone",
        (false, true, ..) => "Firefox",
        (false, _, true, ..) => "Edge",
        (false, _, _, true, _) => "Chrome",
        (false, _, _, _, true) => "Safari",
        _ => "A browser",
    }
}

/// Where the game lists and ends an Ember ID's web desk sessions.
pub const WEB_SESSIONS_PATH: &str = "/v1/web-sessions";

/// `GET /v1/web-sessions` (player session): the Ember ID's web desk
/// sessions still open, newest first, and how many the game has not told
/// the player about yet.
pub async fn web_sessions(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let now = state.now();
    let sessions = state
        .db
        .read(move |tx| {
            let mut statement = tx.prepare(
                "SELECT site, agent, created_at, last_seen, noticed, from_game FROM desk_sessions
                 WHERE ember_id = ?1 AND expires_at > ?2 AND last_seen > ?3
                 ORDER BY created_at DESC LIMIT 20",
            )?;
            let rows = statement.query_map(
                params![
                    player.ember_id.as_str(),
                    now,
                    now.saturating_sub(DESK_IDLE_SECS)
                ],
                |row| {
                    Ok(json!({
                        "site": row.get::<_, String>(0)?,
                        "browser": row.get::<_, String>(1)?,
                        "started_at": row.get::<_, u64>(2)?,
                        "last_used_at": row.get::<_, u64>(3)?,
                        "new": !row.get::<_, bool>(4)?,
                        "origin": if row.get::<_, bool>(5)? { "game" } else { "site" },
                    }))
                },
            )?;
            Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
        })
        .await?;
    Ok(ok(&json!({ "sessions": sessions })))
}

/// `POST /v1/web-sessions/seen` (player session): the game has told the
/// player about the sessions it listed.
pub async fn web_sessions_seen(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    state
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE desk_sessions SET noticed = 1 WHERE ember_id = ?1",
                [player.ember_id.as_str()],
            )?;
            Ok(())
        })
        .await?;
    Ok(ok(&json!({})))
}

/// `DELETE /v1/web-sessions` (player session, `site.remove` proof): signs
/// the Ember ID out of the web desk everywhere.
pub async fn end_web_sessions(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let target = (
        Action::SiteRemove,
        Method::Delete,
        WEB_SESSIONS_PATH.to_owned(),
    );
    let ended = sessions::as_proven(&state, player, &body, target, |tx, _, ember_id| {
        Ok(tx.execute("DELETE FROM desk_sessions WHERE ember_id = ?1", [ember_id])?)
    })
    .await?;
    Ok(ok(&json!({ "ended": ended })))
}

/// A handoff from the game lives this long.
const HANDOFF_SECS: u64 = 120;
/// Handoffs one Ember ID asks for, and exchanges one network address or one
/// code tries, in ten minutes.
const HANDOFFS_PER_PLAYER: usize = 10;
const EXCHANGES_PER_ADDRESS: usize = 10;
const TRIES_PER_CODE: usize = 5;
/// A code's letters: no 0, 1, I or O, which read alike. 32 of them, so a
/// random byte's low five bits pick one evenly.
const CODE_LETTERS: &[u8; 32] = b"ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
pub const HANDOFF_PATH: &str = "/v1/web-sessions/handoff";

/// `POST /v1/web-sessions/handoff` (player session, `site.connect` proof):
/// a one-time handoff that opens the web desk from the game, as a link's
/// token and as an eight-letter code to type on a phone.
pub async fn handoff(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let now = state.now();
    state
        .rate
        .check(
            &format!("desk-handoff:{}", player.ember_id.as_str()),
            HANDOFFS_PER_PLAYER,
            600,
            now,
        )
        .map_err(ApiFailure::rate_limited)?;
    let token = b64u(&random::<32>());
    let code: String = random::<8>()
        .iter()
        .map(|byte| char::from(CODE_LETTERS[usize::from(byte & 31)]))
        .collect();
    let token_hash = auth::hash(&state.keys, "desk-handoff", &token);
    let code_hash = auth::hash(&state.keys, "desk-code", &code);
    let target = (Action::SiteConnect, Method::Post, HANDOFF_PATH.to_owned());
    sessions::as_proven(&state, player, &body, target, move |tx, ctx, ember_id| {
        tx.execute("DELETE FROM desk_handoffs WHERE expires_at <= ?1", [ctx.now])?;
        tx.execute(
            "INSERT INTO desk_handoffs (token_hash, code_hash, ember_id, expires_at) VALUES (?1, ?2, ?3, ?4)",
            params![
                token_hash.as_slice(),
                code_hash.as_slice(),
                ember_id,
                ctx.now + HANDOFF_SECS
            ],
        )?;
        Ok(())
    })
    .await?;
    Ok(answer(
        StatusCode::CREATED,
        &json!({ "token": token, "code": code, "expires_at": now + HANDOFF_SECS }),
    ))
}

/// Uses up the handoff whose `column` hash is `hash` and opens a desk
/// session from the game for it: the cookie, or None when there is none
/// or it lapsed.
async fn exchange(
    state: &AppState,
    headers: &HeaderMap,
    column: &'static str,
    hash: [u8; 32],
) -> Result<Option<String>> {
    let keys = state.keys.clone();
    let agent = agent(headers);
    let now = state.now();
    state
        .db
        .write(move |tx| {
            let ember_id: Option<String> = tx
                .query_row(
                    &format!(
                        "DELETE FROM desk_handoffs WHERE {column} = ?1 AND expires_at > ?2 RETURNING ember_id"
                    ),
                    params![hash.as_slice(), now],
                    |row| row.get(0),
                )
                .optional()?;
            ember_id
                .map(|ember_id| auth::start_desk_session(tx, &keys, &ember_id, "", agent, true, now))
                .transpose()
        })
        .await
}

/// A browser's tries at a handoff, limited per network address.
fn exchange_allowed(state: &AppState, address: &str) -> Result<()> {
    state
        .rate
        .check(
            &format!("desk-exchange:{address}"),
            EXCHANGES_PER_ADDRESS,
            600,
            state.now(),
        )
        .map_err(ApiFailure::rate_limited)
}

/// `GET /desk/h/{token}`: the link the game opens; signs the browser in to
/// the desk from the game.
pub async fn handoff_link(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(token): Path<String>,
    Address(address): Address,
) -> Result<Response> {
    exchange_allowed(&state, &address)?;
    let hash = auth::hash(&state.keys, "desk-handoff", &token);
    match exchange(&state, &headers, "token_hash", hash).await? {
        Some(session) => {
            let mut response = Redirect::to(DESK).into_response();
            set_cookie(&state, &mut response, DESK_COOKIE, &session, DESK_SECS);
            Ok(response)
        }
        None => Ok(finished(
            "Link expired",
            "This link from Ember was used already or is more than two minutes old. \
             Open the desk from Ember again.",
        )),
    }
}

#[derive(serde::Deserialize)]
struct Code {
    code: String,
}

/// `POST /web/handoff` with `{"code": …}`: the code shown in the game,
/// typed on another device; signs that browser in to the desk.
pub async fn handoff_code(
    State(state): State<AppState>,
    headers: HeaderMap,
    Address(address): Address,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    exchange_allowed(&state, &address)?;
    let Code { code } = ember_protocol::json::parse_as(&body.0, PROOF_BODY)?;
    let code: String = code
        .chars()
        .filter(char::is_ascii_alphanumeric)
        .map(|letter| letter.to_ascii_uppercase())
        .collect();
    let refused = || {
        ApiFailure::new(
            ember_protocol::api::ErrorCode::NotFound,
            "That code is wrong or expired. Get a new one in Ember.",
        )
        .detail("reason", "code_refused")
    };
    if code.len() != 8 {
        return Err(refused());
    }
    state
        .rate
        .check(
            &format!("desk-code:{code}"),
            TRIES_PER_CODE,
            600,
            state.now(),
        )
        .map_err(ApiFailure::rate_limited)?;
    let hash = auth::hash(&state.keys, "desk-code", &code);
    let session = exchange(&state, &headers, "code_hash", hash)
        .await?
        .ok_or_else(refused)?;
    let mut response = ok(&json!({}));
    set_cookie(&state, &mut response, DESK_COOKIE, &session, DESK_SECS);
    Ok(response)
}
