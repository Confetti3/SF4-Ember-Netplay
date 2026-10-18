//! A sign-in from Ember on a bridge: Discord's, or a sign-in site's the
//! bridge lists (Challonge, start.gg and any later one; optional, on bridges
//! with the matching feature). The helper asks the bridge to start a sign-in
//! with a proof and opens the site's page in the player's browser itself, so
//! the game never handles the address. The player approves there, and the
//! game reads the account until it appears.
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

/// What differs between the sign-ins: the bridge's routes and proof actions.
pub(super) struct Routes {
    pub start_path: String,
    pub account_path: String,
    pub connect: Action,
    pub remove: Action,
    pub cancel: Action,
}

/// The player's connected account on the bridge, or `account: null`.
pub async fn status(shared: &Arc<Shared>, bridge_id: &str, routes: &Routes) -> Outcome {
    with_session(shared, bridge_id, |bridge, token| async move {
        let (status, body) = shared
            .client
            .call(
                reqwest::Method::GET,
                &format!("{}{}", bridge.origin, routes.account_path),
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

/// Starts a sign-in and opens the site's page in the browser, if
/// `is_sign_in_page` says it is the site's own; anything else the bridge
/// answers is not opened. The account appears once the player finishes
/// there; `status` reads it. With `end_unopened`, a sign-in the bridge
/// started but whose page was not opened is ended at once (best effort), so
/// it holds none of the site's starts.
pub async fn connect(
    shared: &Arc<Shared>,
    bridge_id: &str,
    routes: &Routes,
    is_sign_in_page: impl Fn(&str) -> bool,
    end_unopened: bool,
) -> Outcome {
    let started = proven(
        shared,
        bridge_id,
        routes.connect,
        Method::Post,
        &routes.start_path,
        201,
    )
    .await?;
    let opened = started
        .and_then(|value| serde_json::from_value::<SignInStarted>(value).ok())
        .filter(|started| is_sign_in_page(&started.authorize_url))
        .ok_or_else(|| Failure::new("bridge_invalid_response"))
        .and_then(|started| browser::open(&started.authorize_url));
    if let Err(failure) = opened {
        if end_unopened {
            let _ = cancel(shared, bridge_id, routes).await;
        }
        return Err(failure);
    }
    Ok(Some(json!({ "opened": true })))
}

/// Disconnects the player's account on the bridge.
pub async fn remove(shared: &Arc<Shared>, bridge_id: &str, routes: &Routes) -> Outcome {
    proven(
        shared,
        bridge_id,
        routes.remove,
        Method::Delete,
        &routes.account_path,
        200,
    )
    .await
}

/// Ends the player's sign-ins in flight on the bridge, so a page the player
/// left open connects nothing.
pub async fn cancel(shared: &Arc<Shared>, bridge_id: &str, routes: &Routes) -> Outcome {
    proven(
        shared,
        bridge_id,
        routes.cancel,
        Method::Delete,
        &routes.start_path,
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
    path: &str,
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
