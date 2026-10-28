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
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, cookie, json as answer, ok, page},
    linking::Site,
    routes::{
        organizer::Caller,
        rooms::Address,
        sessions::{Call, Nothing},
    },
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
        && !matches!(
            budget::spend(&state, site.id(), "web-desk", budget::PER_SIGN_IN, limits).await,
            Ok(true)
        )
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
                auth::start_desk_session(tx, &keys, &ember_id, site.id(), &agent, false, now)
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

/// The browser a request came from, as the game shows it: its browser and
/// system, read from its user agent, and nothing that tells one browser from
/// another.
fn agent(headers: &HeaderMap) -> String {
    let agent = headers
        .get(header::USER_AGENT)
        .and_then(|value| value.to_str().ok())
        .unwrap_or_default();
    let has = |text: &str| agent.contains(text);
    // Order matters: Edge, Opera and Samsung's browser also say Chrome, and
    // every Chromium browser also says Safari.
    let browser = if has("Firefox/") || has("FxiOS") {
        "Firefox"
    } else if has("Edg") {
        "Edge"
    } else if has("OPR/") {
        "Opera"
    } else if has("SamsungBrowser") {
        "Samsung Internet"
    } else if has("Chrome/") || has("CriOS") {
        "Chrome"
    } else if has("Safari/") {
        "Safari"
    } else {
        "A browser"
    };
    let system = if has("Android") {
        "Android"
    } else if has("iPhone") || has("iPad") || has("iPod") {
        "iOS"
    } else if has("CrOS") {
        "ChromeOS"
    } else if has("Windows") {
        "Windows"
    } else if has("Macintosh") || has("Mac OS X") {
        "macOS"
    } else if has("Linux") {
        "Linux"
    } else {
        return browser.to_owned();
    };
    format!("{browser} on {system}")
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
    let ended = Call::from(target)
        .run(
            &state,
            &player,
            &body,
            Nothing::check,
            |tx, _, ember_id, _: Nothing| {
                Ok(tx.execute(
                    "DELETE FROM desk_sessions WHERE ember_id = ?1",
                    [ember_id.as_str()],
                )?)
            },
        )
        .await?;
    Ok(ok(&json!({ "ended": ended })))
}

/// A handoff from the game, and a device's code, live this long.
const HANDOFF_SECS: u64 = 120;
/// Handoffs and approvals one Ember ID asks for, codes and exchanges one
/// network address asks for, and reads and approvals one code gets, in ten
/// minutes.
const HANDOFFS_PER_PLAYER: usize = 10;
const EXCHANGES_PER_ADDRESS: usize = 10;
const TRIES_PER_CODE: usize = 5;
/// The cookie holding a device's secret while its code waits.
const DEVICE_COOKIE: &str = "ember_desk_device";
pub const HANDOFF_PATH: &str = "/v1/web-sessions/handoff";
pub const APPROVE_PATH: &str = "/v1/web-sessions/approve";

/// `POST /v1/web-sessions/handoff` (player session, `site.connect` proof):
/// a one-time link's token that opens the web desk in this PC's browser.
pub async fn handoff(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    crate::routes::organizer::invites::require(&state, player.ember_id.as_str()).await?;
    let now = state.now();
    per_player(&state, &player)?;
    let token = b64u(&random::<32>());
    let token_hash = auth::hash(&state.keys, "desk-handoff", &token);
    let target = (Action::SiteConnect, Method::Post, HANDOFF_PATH.to_owned());
    Call::from(target)
        .run(
            &state,
            &player,
            &body,
            Nothing::check,
            move |tx, ctx, ember_id, _: Nothing| {
                tx.execute(
                    "DELETE FROM desk_handoffs WHERE expires_at <= ?1",
                    [ctx.now],
                )?;
                tx.execute(
                "INSERT INTO desk_handoffs (token_hash, ember_id, expires_at) VALUES (?1, ?2, ?3)",
                params![token_hash.as_slice(), ember_id.as_str(), ctx.now + HANDOFF_SECS],
            )?;
                Ok(())
            },
        )
        .await?;
    Ok(answer(
        StatusCode::CREATED,
        &json!({ "token": token, "expires_at": now + HANDOFF_SECS }),
    ))
}

fn per_player(state: &AppState, player: &auth::Player) -> Result<()> {
    state
        .rate
        .check(
            &format!("desk-handoff:{}", player.ember_id.as_str()),
            HANDOFFS_PER_PLAYER,
            600,
            state.now(),
        )
        .map_err(ApiFailure::rate_limited)
}

/// A browser's tries at a handoff or a code, limited per network address.
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
    let (keys, agent, now) = (state.keys.clone(), agent(&headers), state.now());
    let session = state
        .db
        .write(move |tx| {
            let ember_id: Option<String> = tx
                .query_row(
                    "DELETE FROM desk_handoffs WHERE token_hash = ?1 AND expires_at > ?2
                     RETURNING ember_id",
                    params![hash.as_slice(), now],
                    |row| row.get(0),
                )
                .optional()?;
            ember_id
                .map(|ember_id| {
                    auth::start_desk_session(tx, &keys, &ember_id, "", &agent, true, now)
                })
                .transpose()
        })
        .await?;
    match session {
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

/// A code as typed: its digits alone, so spaces and dashes don't matter.
fn typed(code: &str) -> String {
    code.chars().filter(char::is_ascii_digit).collect()
}

/// A code is six digits.
const CODE_DIGITS: usize = 6;

fn code_refused() -> ApiFailure {
    ApiFailure::new(
        ember_protocol::api::ErrorCode::NotFound,
        "That code is wrong or expired. Get a new one on the other device.",
    )
    .detail("reason", "code_refused")
}

/// `POST /web/device`: a new code for this browser to show, to be typed in
/// Ember, and the secret (a cookie) that only this browser holds. What the
/// browser is and when it asked are kept for the game to show.
pub async fn device(
    State(state): State<AppState>,
    headers: HeaderMap,
    Address(address): Address,
) -> Result<Response> {
    exchange_allowed(&state, &address)?;
    let secret = b64u(&random::<32>());
    let secret_hash = auth::hash(&state.keys, "desk-device", &secret);
    let (keys, device, now) = (state.keys.clone(), agent(&headers), state.now());
    let expires_at = now + HANDOFF_SECS;
    let code = state
        .db
        .write(move |tx| {
            tx.execute("DELETE FROM desk_devices WHERE expires_at <= ?1", [now])?;
            // A code another browser holds now is drawn again.
            loop {
                let code = format!(
                    "{:06}",
                    u64::from_le_bytes(random::<8>()) % 10u64.pow(CODE_DIGITS as u32)
                );
                let code_hash = auth::hash(&keys, "desk-code", &code);
                let added = tx.execute(
                    "INSERT OR IGNORE INTO desk_devices
                         (secret_hash, code_hash, expires_at, device, requested_at)
                     VALUES (?1, ?2, ?3, ?4, ?5)",
                    params![
                        secret_hash.as_slice(),
                        code_hash.as_slice(),
                        expires_at,
                        device,
                        now
                    ],
                )?;
                if added == 1 {
                    return Ok(code);
                }
            }
        })
        .await?;
    let mut response = answer(
        StatusCode::CREATED,
        &json!({ "code": code, "expires_at": expires_at }),
    );
    set_cookie(&state, &mut response, DEVICE_COOKIE, &secret, HANDOFF_SECS);
    Ok(response)
}

/// `GET /web/device`: where this browser's code is. Waiting for Ember; read
/// in Ember, with the number to show (`pick`); or approved, which signs the
/// browser in to the desk as opened from the game.
pub async fn device_signed_in(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Response> {
    let secret = cookie(&headers, DEVICE_COOKIE).unwrap_or_default();
    let hash = auth::hash(&state.keys, "desk-device", &secret);
    let (keys, agent, now) = (state.keys.clone(), agent(&headers), state.now());
    let found = state
        .db
        .write(move |tx| {
            let row: Option<(Option<String>, Option<u8>, u64)> = tx
                .query_row(
                    "SELECT ember_id, number, expires_at FROM desk_devices
                     WHERE secret_hash = ?1 AND expires_at > ?2",
                    params![hash.as_slice(), now],
                    |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
                )
                .optional()?;
            match row {
                Some((Some(ember_id), ..)) => {
                    tx.execute(
                        "DELETE FROM desk_devices WHERE secret_hash = ?1",
                        [hash.as_slice()],
                    )?;
                    let session =
                        auth::start_desk_session(tx, &keys, &ember_id, "", &agent, true, now)?;
                    Ok(Some(Ok(session)))
                }
                Some((None, number, expires_at)) => Ok(Some(Err((number, expires_at)))),
                None => Ok(None),
            }
        })
        .await?;
    match found {
        Some(Ok(session)) => {
            let mut response = ok(&json!({ "signed_in": true }));
            set_cookie(&state, &mut response, DESK_COOKIE, &session, DESK_SECS);
            set_cookie(&state, &mut response, DEVICE_COOKIE, "", 0);
            Ok(response)
        }
        Some(Err((Some(number), expires_at))) => {
            Ok(ok(&json!({ "pick": number, "expires_at": expires_at })))
        }
        Some(Err((None, expires_at))) => {
            Ok(ok(&json!({ "waiting": true, "expires_at": expires_at })))
        }
        None => Err(ApiFailure::new(
            ember_protocol::api::ErrorCode::NotFound,
            "This code expired or was turned down. Get a new one.",
        )
        .detail("reason", "code_refused")),
    }
}

/// Where the game reads another device's code before approving it.
pub const DEVICE_PATH: &str = "/v1/web-sessions/device";

#[derive(serde::Deserialize)]
pub struct CodeQuery {
    code: String,
}

/// The number a device shows and two others, in a random order, for the
/// organizer to pick from in the game.
fn number_and_choices() -> (u8, [u8; 3]) {
    let draw = || 10 + random::<1>()[0] % 90;
    let number = draw();
    let mut choices = [number, 0, 0];
    for at in 1..3 {
        choices[at] = loop {
            let other = draw();
            if !choices[..at].contains(&other) {
                break other;
            }
        };
    }
    let first = usize::from(random::<1>()[0] % 3);
    choices.swap(0, first);
    (number, choices)
}

/// `GET /v1/web-sessions/device?code=` (player session): what asked for
/// `code` and when, and three numbers, one of which the device now shows.
/// The first Ember ID to read a code keeps it: another's game gets no answer.
pub async fn device_info(
    State(state): State<AppState>,
    headers: HeaderMap,
    axum::extract::Query(CodeQuery { code }): axum::extract::Query<CodeQuery>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    per_player(&state, &player)?;
    let code = typed(&code);
    if code.len() != CODE_DIGITS {
        return Err(code_refused());
    }
    code_tries(&state, &code)?;
    let hash = auth::hash(&state.keys, "desk-code", &code);
    let (ember_id, now) = (player.ember_id.as_str().to_owned(), state.now());
    let found = state
        .db
        .write(move |tx| {
            let row: Option<(String, u64, Option<String>, Option<String>)> = tx
                .query_row(
                    "SELECT device, requested_at, claimed_by, choices FROM desk_devices
                     WHERE code_hash = ?1 AND expires_at > ?2 AND ember_id IS NULL",
                    params![hash.as_slice(), now],
                    |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
                )
                .optional()?;
            let Some((device, requested_at, claimed_by, choices)) = row else {
                return Ok(None);
            };
            if claimed_by
                .as_deref()
                .is_some_and(|claimed| claimed != ember_id)
            {
                return Ok(None);
            }
            let choices = match choices {
                Some(choices) => choices,
                None => {
                    let (number, choices) = number_and_choices();
                    let choices = choices.map(|choice| choice.to_string()).join(",");
                    tx.execute(
                        "UPDATE desk_devices SET claimed_by = ?1, number = ?2, choices = ?3
                         WHERE code_hash = ?4",
                        params![ember_id, number, choices, hash.as_slice()],
                    )?;
                    choices
                }
            };
            Ok(Some((device, requested_at, choices)))
        })
        .await?;
    let Some((device, requested_at, choices)) = found else {
        return Err(code_refused());
    };
    let choices: Vec<u8> = choices.split(',').filter_map(|n| n.parse().ok()).collect();
    Ok(ok(&json!({
        "device": device,
        "requested_at": requested_at,
        "choices": choices,
    })))
}

/// A code's tries at being read or approved, however many games ask.
fn code_tries(state: &AppState, code: &str) -> Result<()> {
    state
        .rate
        .check(
            &format!("desk-code:{code}"),
            TRIES_PER_CODE,
            600,
            state.now(),
        )
        .map_err(ApiFailure::rate_limited)
}

#[derive(serde::Deserialize)]
struct Approval {
    code: String,
    number: u8,
}

/// `POST /v1/web-sessions/approve` (player session, `site.connect` proof):
/// approves the code another device's browser shows, with the number that
/// browser shows now (`device_info`), so that browser, and only it, signs in
/// to the desk as opened from the game. A wrong number ends the code.
pub async fn approve(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, Approval { code, number }): (_, Approval) =
        Caller::organizer(&state, &headers, &body).await?;
    caller.require_game()?;
    state
        .rate
        .check(
            &format!("desk-handoff:{}", caller.ember_id()),
            HANDOFFS_PER_PLAYER,
            600,
            state.now(),
        )
        .map_err(ApiFailure::rate_limited)?;
    let code = typed(&code);
    if code.len() != CODE_DIGITS {
        return Err(code_refused());
    }
    code_tries(&state, &code)?;
    let hash = auth::hash(&state.keys, "desk-code", &code);
    let ctx = Ctx::of(&state);
    let approved = state
        .db
        .write(move |tx| {
            caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, APPROVE_PATH)?;
            let shown: Option<u8> = tx
                .query_row(
                    "SELECT number FROM desk_devices
                     WHERE code_hash = ?1 AND expires_at > ?2 AND ember_id IS NULL
                         AND claimed_by = ?3",
                    params![hash.as_slice(), ctx.now, caller.ember_id()],
                    |row| row.get(0),
                )
                .optional()?
                .flatten();
            match shown {
                None => Ok(None),
                Some(shown) if shown != number => {
                    tx.execute(
                        "DELETE FROM desk_devices WHERE code_hash = ?1",
                        [hash.as_slice()],
                    )?;
                    Ok(Some(false))
                }
                Some(_) => {
                    tx.execute(
                        "UPDATE desk_devices SET ember_id = ?1 WHERE code_hash = ?2",
                        params![caller.ember_id(), hash.as_slice()],
                    )?;
                    Ok(Some(true))
                }
            }
        })
        .await?;
    match approved {
        Some(true) => Ok(ok(&json!({ "approved": true }))),
        Some(false) => Err(ApiFailure::new(
            ember_protocol::api::ErrorCode::NotFound,
            "That is not the number the other device shows, so its code is ended. \
             Get a new code there.",
        )
        .detail("reason", "number_wrong")),
        None => Err(code_refused()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn read(agent: &str) -> String {
        let mut headers = HeaderMap::new();
        headers.insert(header::USER_AGENT, HeaderValue::from_str(agent).unwrap());
        super::agent(&headers)
    }

    #[test]
    fn a_device_is_named_by_its_browser_and_system() {
        assert_eq!(
            read(
                "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0 Mobile Safari/537.36"
            ),
            "Chrome on Android"
        );
        assert_eq!(
            read(
                "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1"
            ),
            "Safari on iOS"
        );
        assert_eq!(
            read(
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0 Safari/537.36 Edg/129.0"
            ),
            "Edge on Windows"
        );
        assert_eq!(read("curl/8"), "A browser");
    }

    #[test]
    fn the_number_is_among_three_distinct_choices() {
        for _ in 0..200 {
            let (number, choices) = number_and_choices();
            assert!(choices.contains(&number));
            assert!(choices.iter().all(|choice| (10..100).contains(choice)));
            assert!(
                choices[0] != choices[1] && choices[1] != choices[2] && choices[0] != choices[2]
            );
        }
    }
}
