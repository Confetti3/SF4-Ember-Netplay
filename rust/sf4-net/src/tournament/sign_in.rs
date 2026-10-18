//! A site's sign-in from Ember, for the sites a bridge connects accounts
//! from (Discord, Challonge; optional, on bridges that list the site's
//! feature). The helper asks the bridge to start a sign-in with a proof and
//! opens the site's page in the player's browser itself, so the game never
//! handles the address. The player approves there, and the game reads the
//! account until it appears.
use std::sync::Arc;

use ember_protocol::{
    challenge::{Action, Method},
    sign_in::{Connection, SignInStarted},
};
use serde_json::json;

use super::{
    Failure, Outcome, Shared, browser,
    client::{answer, prove, with_session},
};

/// What differs between the sites: the bridge's routes and proof actions,
/// and the pages the helper may open.
pub(super) trait Site {
    const START_PATH: &'static str;
    const ACCOUNT_PATH: &'static str;
    const CONNECT: Action;
    const REMOVE: Action;
    const CANCEL: Action;
    /// The site's own sign-in page, with nothing that could break out of a
    /// URL. Anything else the bridge answers is not opened.
    fn is_sign_in_page(url: &str) -> bool;
}

/// The player's connected account on the bridge, or `account: null`.
pub async fn status<S: Site>(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    with_session(shared, bridge_id, |bridge, token| async move {
        let (status, body) = shared
            .client
            .call(
                reqwest::Method::GET,
                &format!("{}{}", bridge.origin, S::ACCOUNT_PATH),
                Some(&token),
                None,
            )
            .await?;
        let connection: Connection = serde_json::from_value(answer(status, &body, &[200])?)
            .map_err(|_| Failure::new("bridge_invalid_response"))?;
        Ok(Some(json!(connection)))
    })
    .await
}

/// Starts a sign-in and opens the site's page in the browser. The account
/// appears once the player finishes there; `status` reads it.
pub async fn connect<S: Site>(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    let started = proven(
        shared,
        bridge_id,
        S::CONNECT,
        Method::Post,
        S::START_PATH,
        201,
    )
    .await?;
    let started: SignInStarted = started
        .and_then(|value| serde_json::from_value(value).ok())
        .filter(|started: &SignInStarted| S::is_sign_in_page(&started.authorize_url))
        .ok_or_else(|| Failure::new("bridge_invalid_response"))?;
    browser::open(&started.authorize_url)?;
    Ok(Some(json!({ "opened": true })))
}

/// Disconnects the player's account on the bridge.
pub async fn remove<S: Site>(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    proven(
        shared,
        bridge_id,
        S::REMOVE,
        Method::Delete,
        S::ACCOUNT_PATH,
        200,
    )
    .await
}

/// Ends the player's sign-ins in flight on the bridge, so a page the player
/// left open connects nothing.
pub async fn cancel<S: Site>(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    proven(
        shared,
        bridge_id,
        S::CANCEL,
        Method::Delete,
        S::START_PATH,
        200,
    )
    .await
}

/// An empty command for `action` at `method path`, proven and sent, and the
/// bridge's answer when it is `expected`.
async fn proven(
    shared: &Arc<Shared>,
    bridge_id: &str,
    action: Action,
    method: Method,
    path: &'static str,
    expected: u16,
) -> Outcome {
    let verb = match method {
        Method::Post => reqwest::Method::POST,
        Method::Delete => reqwest::Method::DELETE,
    };
    with_session(shared, bridge_id, |bridge, token| {
        let verb = verb.clone();
        async move {
            let body = prove(
                shared,
                &bridge,
                Some(&token),
                action,
                method,
                path,
                json!({}),
            )
            .await?;
            let (status, response) = shared
                .client
                .call(
                    verb,
                    &format!("{}{path}", bridge.origin),
                    Some(&token),
                    Some(body),
                )
                .await?;
            Ok(Some(answer(status, &response, &[expected])?))
        }
    })
    .await
}
