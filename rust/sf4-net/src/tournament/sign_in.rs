//! A sign-in from Ember on a bridge: Discord's, or a sign-in site's the
//! bridge lists (Challonge, start.gg and any later one; optional, on bridges
//! with the matching feature). The helper asks the bridge to start a sign-in
//! with a proof and opens the site's page in the player's browser itself, so
//! the game never handles the address. The player approves there, and the
//! game reads the account until it appears.
use std::sync::Arc;

use ember_protocol::{
    challenge::Action,
    sign_in::{Connection, SignInStarted},
};
use serde_json::json;

use super::{
    Code, Failure, Outcome, Shared, browser,
    client::{self, Body, BridgeCall, Verb},
};

/// What differs between the sign-ins: the bridge's routes and proof actions,
/// and the page the player is sent to.
pub(super) struct Routes {
    pub start_path: String,
    pub account_path: String,
    pub connect: Action,
    pub remove: Action,
    pub cancel: Action,
    /// A sign-in the bridge started but whose page was not opened is ended
    /// at once (best effort), so it holds none of the site's starts.
    pub end_unopened: bool,
    /// Whether an address the bridge answers is the sign-in's own page, which
    /// is the only kind opened.
    pub is_sign_in_page: Box<dyn Fn(&str) -> bool + Send + Sync>,
}

/// The player's connected account on the bridge, or `account: null`.
pub async fn status(shared: &Arc<Shared>, bridge_id: &str, routes: &Routes) -> Outcome {
    let call = BridgeCall::new(Verb::Get, &routes.account_path, &[200]);
    let connection: Connection = client::request_as(shared, bridge_id, &call).await?;
    Ok(Some(json!(connection)))
}

/// The player's organizer grant on the bridge: `organizer` is its scope and
/// expiry, or null when none is held. The tokens never leave the bridge.
pub async fn grant(shared: &Arc<Shared>, bridge_id: &str, routes: &Routes) -> Outcome {
    #[derive(serde::Deserialize, serde::Serialize)]
    struct Grant {
        scope: String,
        expires_at: u64,
    }
    #[derive(serde::Deserialize)]
    struct Held {
        organizer: Option<Grant>,
    }
    let call = BridgeCall::new(Verb::Get, &routes.account_path, &[200]);
    let held: Held = client::request_as(shared, bridge_id, &call).await?;
    Ok(Some(json!({ "organizer": held.organizer })))
}

/// Starts a sign-in and opens the site's page in the browser, if
/// `Routes::is_sign_in_page` says it is the site's own; anything else the
/// bridge answers is not opened. The account appears once the player finishes
/// there; `status` reads it. The routes say whether a sign-in whose page
/// was not opened is ended (`Routes::end_unopened`).
pub async fn connect(shared: &Arc<Shared>, bridge_id: &str, routes: &Routes) -> Outcome {
    let started = proven(
        shared,
        bridge_id,
        routes.connect,
        Verb::Post,
        &routes.start_path,
        201,
    )
    .await?;
    let opened = started
        .and_then(|value| serde_json::from_value::<SignInStarted>(value).ok())
        .filter(|started| (routes.is_sign_in_page)(&started.authorize_url))
        .ok_or_else(|| Failure::new(Code::BridgeInvalidResponse))
        .and_then(|started| browser::open(&started.authorize_url));
    if let Err(failure) = opened {
        if routes.end_unopened {
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
        Verb::Delete,
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
        Verb::Delete,
        &routes.start_path,
        200,
    )
    .await
}

/// An empty command for `action` at `verb path`, proven and sent, and the
/// bridge's answer when it is `expected`.
async fn proven(
    shared: &Arc<Shared>,
    bridge_id: &str,
    action: Action,
    verb: Verb,
    path: &str,
    expected: u16,
) -> Outcome {
    let command = json!({});
    let call = BridgeCall::new(verb, path, std::slice::from_ref(&expected)).body(Body::Proven {
        action,
        command: &command,
    });
    client::request(shared, bridge_id, &call).await.map(Some)
}
