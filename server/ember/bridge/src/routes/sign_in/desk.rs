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
    http::{HeaderMap, HeaderValue, header},
    response::{IntoResponse, Redirect, Response},
};
use ember_protocol::{encoding::b64u, sign_in::SIGN_IN_SECS};
use rusqlite::params;
use serde_json::json;
use subtle::ConstantTimeEq;

use super::{Answer, OAuth, SignedIn, budget, configured, offered, redirect_uri};
use crate::{
    AppState,
    auth::{self, DESK_COOKIE, DESK_SECS},
    error::{ApiFailure, Result},
    http::{cookie, ok, page},
    linking::Site,
    util::{html, random},
};

/// Starts a web sign-in's state, after which comes its secret.
pub(super) const WEB: &str = "web.";

/// The cookie that ties a sign-in to the browser that started it.
const STATE_COOKIE: &str = "ember_desk_state";

/// Web sign-ins started on one site per hour, across the bridge.
const STARTS_PER_HOUR: usize = 60;

/// Where a signed-in browser goes.
const DESK: &str = "/desk";

fn state_hash(state: &AppState, site: &Site, secret: &str) -> [u8; 32] {
    state
        .keys
        .keyed_hash(&site.domain("desk-sign-in"), secret.as_bytes())
}

/// `GET /web/sign-in/{site}`: sends the browser to the site to sign in.
pub async fn start(State(state): State<AppState>, Path(id): Path<String>) -> Result<Response> {
    let site = configured(&state, &id)?;
    let oauth = OAuth::of(&state, &site)
        .filter(|_| offered(&state, &site))
        .ok_or_else(ApiFailure::not_found)?;
    let now = state.now();
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
    let session = state
        .db
        .write(move |tx| match site.owner(tx, &user.id)? {
            Some(ember_id) => auth::start_desk_session(tx, &keys, &ember_id, now).map(Some),
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
    Ok(ok(&json!({ "ember_id": desk.ember_id, "csrf": desk.csrf })))
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
