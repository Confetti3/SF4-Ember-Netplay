//! Friends for the native side (docs/design/FRIENDS.md, "Wire contracts"):
//! the player's presence in and the whole `FriendsState` out, and one action
//! on the list with the state after it. Like the ranking routes these use
//! the player session alone, and what the bridge answers is checked before
//! the game sees it. A bridge that does not list `friends` is never asked,
//! and is told apart as `friends_unavailable`, as one that answers 404 is.
use std::sync::Arc;

use super::{
    Code, Failure, Outcome, Shared,
    client::{self, Body, BridgeCall, Refusals, Verb},
    play::{self, Need},
};
use ember_protocol::{
    friends::{
        ACTIONS_PATH, CARD_EXPIRED, CODE_TOO_SOON, FRIENDS_FULL, FriendAction, FriendsState,
        FriendsSync, INVITE_UNAVAILABLE, OWN_CARD, REQUEST_LIMIT, SYNC_PATH, UNKNOWN_CODE,
    },
    skill::NO_ACCOUNT,
};

/// Refusals of the friends routes that the game words apart.
const REASONS: [&str; 8] = [
    NO_ACCOUNT,
    UNKNOWN_CODE,
    CARD_EXPIRED,
    OWN_CARD,
    FRIENDS_FULL,
    REQUEST_LIMIT,
    CODE_TOO_SOON,
    INVITE_UNAVAILABLE,
];
/// A 404 naming none of them is a bridge without the routes.
const REFUSALS: Refusals<'static> = Refusals {
    reasons: &REASONS,
    missing: Some(Code::FriendsUnavailable),
};

/// The bridge's capabilities list `friends`, or this is
/// `friends_unavailable` and nothing is asked of it. One that lacks the
/// feature is read again on the next request, so a bridge upgraded meanwhile
/// is heard.
async fn offered(shared: &Arc<Shared>, bridge_id: &str) -> Result<(), Failure> {
    let bridge = client::approved(shared, bridge_id)?;
    if play::features(shared, &bridge, Need::Friends).await?.friends {
        Ok(())
    } else {
        Err(Failure::new(Code::FriendsUnavailable))
    }
}

/// Sends the player's presence and answers the state it gets back.
pub async fn sync(shared: &Arc<Shared>, bridge_id: &str, sync: FriendsSync) -> Outcome {
    sync.check()
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    let body = serde_json::to_vec(&sync).map_err(|_| Failure::new(Code::Internal))?;
    exchange(shared, bridge_id, SYNC_PATH, &body).await
}

/// Carries out one action and answers the state after it.
pub async fn act(shared: &Arc<Shared>, bridge_id: &str, action: FriendAction) -> Outcome {
    action
        .check()
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    let body = serde_json::to_vec(&action).map_err(|_| Failure::new(Code::Internal))?;
    exchange(shared, bridge_id, ACTIONS_PATH, &body).await
}

/// Both routes answer the whole state, which is checked before it is passed on.
async fn exchange(shared: &Arc<Shared>, bridge_id: &str, path: &str, body: &[u8]) -> Outcome {
    offered(shared, bridge_id).await?;
    let call = BridgeCall::new(Verb::Post, path, &[200])
        .body(Body::Json(body))
        .refusals(REFUSALS);
    let state: FriendsState = client::request_as(shared, bridge_id, &call).await?;
    state
        .check()
        .map_err(|_| Failure::new(Code::BridgeInvalidResponse))?;
    serde_json::to_value(&state)
        .map(Some)
        .map_err(|_| Failure::new(Code::Internal))
}

#[cfg(test)]
mod tests {
    use serde_json::{Value, json};

    use super::*;
    use crate::tournament::{Lane, Request, test_support::shared_pinning};

    const FRIEND: &str = "fr_11111111-1111-4111-8111-111111111111";
    const REQUEST: &str = "rq_11111111-1111-4111-8111-111111111111";
    const TOKEN: &str = "c1.AbCdEf_-0123";

    fn reply(status: u16, body: &[u8]) -> Result<Value, Failure> {
        client::answer(status, body, &[200], &REFUSALS)
    }

    fn refusal(code: &str, reason: Option<&str>) -> Vec<u8> {
        let details = reason.map_or(json!({}), |reason| json!({ "reason": reason }));
        serde_json::to_vec(&json!({ "error": {
            "code": code, "message": "m", "retryable": false, "request_id": "req_1", "details": details,
        }}))
        .unwrap()
    }

    fn sync_request() -> Value {
        json!({ "op": "friends_sync", "bridge_id": "brg_x", "sync": {
            "build_id": "b1", "listing": { "name": "Player 1", "country": "GB" }, "main": 3,
            "presence": { "activity": "room",
                "room": { "room_id": "0f".repeat(16), "status": "fighting", "table": 0 } } } })
    }

    #[test]
    fn requests_have_the_shapes_the_game_sends() {
        let actions = [
            json!({ "action": "request", "card": TOKEN, "label": "Player 1's room" }),
            json!({ "action": "request", "code": "4F2A-91CX" }),
            json!({ "action": "accept", "request": REQUEST }),
            json!({ "action": "decline", "request": REQUEST, "block": true }),
            json!({ "action": "cancel", "request": REQUEST }),
            json!({ "action": "remove", "friend": FRIEND }),
            json!({ "action": "block", "card": TOKEN, "label": "Player 2" }),
            json!({ "action": "unblock", "block": "bl_11111111-1111-4111-8111-111111111111" }),
            json!({ "action": "invite", "friend": FRIEND, "room": { "room_id": "0f".repeat(16) } }),
            json!({ "action": "dismiss", "invite": "iv_11111111-1111-4111-8111-111111111111" }),
            json!({ "action": "appear_offline", "on": true }),
            json!({ "action": "new_code" }),
            json!({ "action": "forget" }),
        ];
        let mut requests = vec![(sync_request(), "friends_sync")];
        requests.extend(actions.into_iter().map(|action| {
            (
                json!({ "op": "friends_act", "bridge_id": "brg_x", "act": action }),
                "friends_act",
            )
        }));
        for (value, op) in requests {
            let parsed: Request = serde_json::from_value(value.clone()).expect("a request");
            assert_eq!(parsed.op(), op);
            assert_eq!(parsed.lane(), Lane::Bridge);
            assert_eq!(serde_json::to_value(&parsed).unwrap(), value);
        }
    }

    #[test]
    fn requests_with_unknown_members_or_actions_are_refused() {
        let mut with_extra = sync_request();
        with_extra["extra"] = json!(1);
        let mut in_sync = sync_request();
        in_sync["sync"]["extra"] = json!(1);
        let mut in_room = sync_request();
        in_room["sync"]["presence"]["room"]["extra"] = json!(1);
        let mut without_presence = sync_request();
        without_presence["sync"].as_object_mut().unwrap().remove("presence");
        let act = |action: Value| json!({ "op": "friends_act", "bridge_id": "brg_x", "act": action });
        for bad in [
            with_extra,
            in_sync,
            in_room,
            without_presence,
            json!({ "op": "friends_sync", "bridge_id": "brg_x" }),
            json!({ "op": "friends_act", "bridge_id": "brg_x" }),
            act(json!({ "action": "befriend" })),
            act(json!({ "action": "accept", "request": REQUEST, "extra": 1 })),
            act(json!({ "action": "accept" })),
            act(json!({ "action": "new_code", "extra": 1 })),
            act(json!({ "action": "appear_offline" })),
            act(json!({ "action": "invite", "friend": FRIEND, "room": { "room_id": "a", "secret": "b" } })),
        ] {
            assert!(
                serde_json::from_value::<Request>(bad.clone()).is_err(),
                "{bad}"
            );
        }
    }

    // What the bridge would refuse is refused before any bridge is asked.
    #[tokio::test]
    async fn a_malformed_request_is_refused_before_it_is_asked() {
        let dir = std::env::temp_dir().join(format!("sf4-net-friends-{}", std::process::id()));
        let shared = shared_pinning(&dir, &[]);
        let sync_of = |text: Value| -> FriendsSync { serde_json::from_value(text).unwrap() };
        let mut bad_main = sync_request()["sync"].clone();
        bad_main["main"] = json!(44);
        let refused_sync = |sync: FriendsSync| {
            let shared = shared.clone();
            async move {
                super::sync(&shared, "brg_never_approved", sync)
                    .await
                    .unwrap_err()
                    .as_str()
                    .to_owned()
            }
        };
        assert_eq!(refused_sync(sync_of(bad_main)).await, "invalid_request");
        assert_eq!(
            refused_sync(sync_of(sync_request()["sync"].clone())).await,
            "bridge_not_approved"
        );
        let refused_act = |action: Value| {
            let shared = shared.clone();
            async move {
                act(&shared, "brg_never_approved", serde_json::from_value(action).unwrap())
                    .await
                    .unwrap_err()
                    .as_str()
                    .to_owned()
            }
        };
        // A request names a card with its label or a code, never both or neither.
        for bad in [
            json!({ "action": "request" }),
            json!({ "action": "request", "card": TOKEN }),
            json!({ "action": "request", "card": TOKEN, "label": "x", "code": "4F2A-91CX" }),
            json!({ "action": "request", "code": "4F2A-91C" }),
            json!({ "action": "accept", "request": FRIEND }),
            json!({ "action": "remove", "friend": REQUEST }),
        ] {
            assert_eq!(refused_act(bad.clone()).await, "invalid_request", "{bad}");
        }
        assert_eq!(
            refused_act(json!({ "action": "new_code" })).await,
            "bridge_not_approved"
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_bridge_without_friends_is_told_apart() {
        assert_eq!(reply(404, b"").unwrap_err().as_str(), "friends_unavailable");
        let body = refusal("not_found", None);
        assert_eq!(
            reply(404, &body).unwrap_err().as_str(),
            "friends_unavailable"
        );
        for reason in REASONS {
            let body = refusal("forbidden", Some(reason));
            assert_eq!(reply(409, &body).unwrap_err().as_str(), reason);
            // A route that names a reason is never mistaken for a missing one.
            assert_eq!(reply(404, &body).unwrap_err().as_str(), reason);
        }
        let body = refusal("rate_limited", None);
        assert_eq!(reply(429, &body).unwrap_err().as_str(), "rate_limited");
        assert_eq!(reply(503, b"").unwrap_err().as_str(), "http_503");
    }

    #[test]
    fn the_state_is_read_strictly() {
        let state = json!({ "code": "4F2A-91CX", "friends": [], "incoming": [],
            "outgoing": [], "invites": [], "blocked": [] });
        let read = |value: &Value| -> Result<FriendsState, Failure> {
            client::read(200, &serde_json::to_vec(value).unwrap(), &[200], &REFUSALS)
        };
        assert!(read(&state).unwrap().check().is_ok());
        let mut with_extra = state.clone();
        with_extra["extra"] = json!(1);
        let mut without_code = state.clone();
        without_code.as_object_mut().unwrap().remove("code");
        let mut in_friend = state.clone();
        in_friend["friends"] = json!([{ "id": FRIEND, "name": "Player 3", "since": 1,
            "presence": { "state": "offline" }, "extra": 1 }]);
        let mut unknown_state = state.clone();
        unknown_state["friends"] = json!([{ "id": FRIEND, "name": "Player 3", "since": 1,
            "presence": { "state": "away" } }]);
        for bad in [with_extra, without_code, in_friend, unknown_state] {
            assert_eq!(
                read(&bad).unwrap_err().as_str(),
                "bridge_invalid_response",
                "{bad}"
            );
        }
        // A state that parses but breaks a rule does not check.
        let mut bad_code = state;
        bad_code["code"] = json!("4F2A91CX");
        assert!(read(&bad_code).unwrap().check().is_err());
    }
}
