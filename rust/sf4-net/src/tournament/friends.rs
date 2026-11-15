//! Friends for the native side (docs/design/FRIENDS.md, "Wire contracts"):
//! the player's presence in and the whole `FriendsState` out, and one action
//! on the list with the state after it. Like the ranking routes these use
//! the player session alone, and what the bridge answers is checked before
//! the game sees it. A bridge that does not list `friends` is never asked,
//! and is told apart as `friends_unavailable`, as one that answers 404 is.
//!
//! A bridge that lists `friends` but not `friends_presence_v2` refuses a
//! sync's `game`, a room's `offer` and the `join` action, so they are not
//! sent to it: the sync goes without them, and a join is refused here as the
//! bridge would, `join_unavailable`. The answer is read and written back
//! through the protocol types, so what a newer bridge adds to it (`game`,
//! `private_room`, `joining`) reaches the game as it was sent.
use std::sync::Arc;

use super::{
    Code, Failure, Outcome, Refusal, Shared,
    client::{self, Body, BridgeCall, Refusals, Verb},
    play::{self, Need},
};
use ember_protocol::{
    api::ErrorCode,
    friends::{
        ACTIONS_PATH, FriendAction, FriendRefusal, FriendsState, FriendsSync, PresenceReport,
        SYNC_PATH,
    },
};

/// Refusals of the friends routes that the game words apart: every reason the
/// bridge writes (`FriendRefusal`). A 404 naming none of them is a bridge
/// without the routes.
const REFUSALS: Refusals<'static> = Refusals {
    reasons: &FriendRefusal::NAMES,
    missing: Some(Code::FriendsUnavailable),
};

/// Sends the player's presence and answers the state it gets back.
pub async fn sync(shared: &Arc<Shared>, bridge_id: &str, mut sync: FriendsSync) -> Outcome {
    drop_unreadable_offer(&mut sync);
    sync.check()
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    let offered = play::require(shared, bridge_id, Need::Friends).await?;
    if !offered.friends_presence_v2 {
        strip_presence_v2(&mut sync);
    }
    let carries = carries_presence_v2(&sync);
    let body = serde_json::to_vec(&sync).map_err(|_| Failure::new(Code::Internal))?;
    exchange(shared, bridge_id, SYNC_PATH, &body, carries).await
}

/// Carries out one action and answers the state after it.
pub async fn act(shared: &Arc<Shared>, bridge_id: &str, action: FriendAction) -> Outcome {
    action
        .check()
        .map_err(|_| Failure::new(Code::InvalidRequest))?;
    let offered = play::require(shared, bridge_id, Need::Friends).await?;
    let joins = matches!(action, FriendAction::Join { .. });
    if joins && !offered.friends_presence_v2 {
        return Err(join_unavailable());
    }
    let body = serde_json::to_vec(&action).map_err(|_| Failure::new(Code::Internal))?;
    exchange(shared, bridge_id, ACTIONS_PATH, &body, joins).await
}

/// A sync as a bridge without `friends_presence_v2` reads it: no `game` and
/// no private room `offer`. What is left is a sync that bridge accepted
/// before the members existed.
fn strip_presence_v2(sync: &mut FriendsSync) {
    sync.game = None;
    if let PresenceReport::Room { room } = &mut sync.presence {
        room.offer = None;
    }
}

/// An offer the bridge would refuse goes without failing the sync it rides in:
/// the player is still in their private room, and their friends list still
/// refreshes. A room's name is checked more loosely by the room than by the
/// bridge (a trailing space is a name to one and not to the other), and the
/// offer is the only part of a presence that carries it.
fn drop_unreadable_offer(sync: &mut FriendsSync) {
    if let PresenceReport::Room { room } = &mut sync.presence {
        if room
            .offer
            .as_ref()
            .is_some_and(|offer| offer.check().is_err())
        {
            room.offer = None;
        }
    }
}

/// Whether `sync` names a member only a bridge with `friends_presence_v2`
/// reads.
fn carries_presence_v2(sync: &FriendsSync) -> bool {
    sync.game.is_some()
        || matches!(&sync.presence, PresenceReport::Room { room } if room.offer.is_some())
}

/// The refusal of a join the bridge could not be asked, worded as the bridge
/// words it, so the game handles one case.
fn join_unavailable() -> Failure {
    Failure::Bridge(Refusal::Reason(
        FriendRefusal::JoinUnavailable.as_str().into(),
    ))
}

/// Whether a bridge refused a request as malformed, as one does that does not
/// read a member of it: its `invalid_request`, or a 400 that names none. A
/// reason a route words apart (`own_card`) is another refusal.
fn refused_as_malformed(failure: &Failure) -> bool {
    matches!(
        failure,
        Failure::Bridge(refusal)
            if refusal.is(ErrorCode::InvalidRequest) || matches!(refusal, Refusal::Status(400))
    )
}

/// Both routes answer the whole state, which is checked before it is passed on.
/// `carries` says the request held a member of `friends_presence_v2`: when it
/// is refused as malformed, the bridge may have been put back to one that
/// does not read it, so what it offers is read again with the next call
/// (`play::forget_features`), as a claim does.
async fn exchange(
    shared: &Arc<Shared>,
    bridge_id: &str,
    path: &str,
    body: &[u8],
    carries: bool,
) -> Outcome {
    let call = BridgeCall::new(Verb::Post, path, &[200])
        .body(Body::Json(body))
        .refusals(REFUSALS);
    let state: FriendsState = match client::request_as(shared, bridge_id, &call).await {
        Ok(state) => state,
        Err(failure) => {
            if carries && refused_as_malformed(&failure) {
                let bridge = client::approved(shared, bridge_id)?;
                play::forget_features(shared, &bridge)?;
            }
            return Err(failure);
        }
    };
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
            json!({ "action": "join", "friend": FRIEND }),
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
            json!({ "action": "turn_on" }),
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

    fn offering_sync() -> FriendsSync {
        serde_json::from_value(json!({ "build_id": "b1", "game": "usf4",
            "listing": { "name": "Player 1" },
            "presence": { "activity": "room", "room": { "private": true, "status": "idle",
                "offer": { "secret": format!("emd1:{}", "A".repeat(122)),
                           "name": "Lounge", "members": 2, "capacity": 8 } } } }))
        .unwrap()
    }

    // An offer whose room name the bridge would refuse is left out, and the
    // sync goes as a private room's without one.
    #[test]
    fn an_offer_the_bridge_would_refuse_is_dropped_and_the_sync_still_goes() {
        for name in ["Lounge ", " Lounge", "Lounge\u{3000}", ""] {
            let mut sync = offering_sync();
            if let PresenceReport::Room { room } = &mut sync.presence {
                room.offer.as_mut().unwrap().name = name.to_owned();
            }
            assert!(sync.check().is_err(), "{name:?}");
            drop_unreadable_offer(&mut sync);
            assert!(sync.check().is_ok(), "{name:?}");
            assert_eq!(
                serde_json::to_value(&sync).unwrap()["presence"],
                json!({ "activity": "room", "room": { "private": true, "status": "idle" } })
            );
        }
        // A well-formed offer stays.
        let mut sync = offering_sync();
        drop_unreadable_offer(&mut sync);
        assert!(matches!(&sync.presence, PresenceReport::Room { room } if room.offer.is_some()));
    }

    // A bridge without `friends_presence_v2` is sent the sync it read before:
    // no game and no offer, and the rest as it was.
    #[test]
    fn a_sync_goes_without_the_presence_members_to_a_bridge_that_lacks_them() {
        let mut sync = offering_sync();
        assert!(carries_presence_v2(&sync));
        strip_presence_v2(&mut sync);
        assert!(!carries_presence_v2(&sync));
        assert!(sync.check().is_ok());
        assert_eq!(
            serde_json::to_value(&sync).unwrap(),
            json!({ "build_id": "b1", "listing": { "name": "Player 1" },
                "presence": { "activity": "room",
                    "room": { "private": true, "status": "idle" } } })
        );
        // A game alone, or an offer alone, is a v2 member each.
        let mut game_only = offering_sync();
        if let PresenceReport::Room { room } = &mut game_only.presence {
            room.offer = None;
        }
        assert!(carries_presence_v2(&game_only));
        let mut offer_only = offering_sync();
        offer_only.game = None;
        assert!(carries_presence_v2(&offer_only));
        // The menus have nothing to strip.
        let mut menus: FriendsSync = serde_json::from_value(json!({ "build_id": "b1",
            "listing": { "name": "Player 1" }, "presence": { "activity": "menus" } }))
        .unwrap();
        let before = menus.clone();
        strip_presence_v2(&mut menus);
        assert_eq!(menus, before);
        assert!(!carries_presence_v2(&menus));
    }

    // A join never reaches a bridge that cannot read it: the game gets the
    // refusal it words, in the shape of the bridge's own.
    #[test]
    fn a_join_is_refused_here_as_the_bridge_refuses_it() {
        assert_eq!(join_unavailable().as_str(), "join_unavailable");
        assert!(FriendRefusal::NAMES.contains(&"join_unavailable"));
        let from_bridge =
            reply(409, &refusal("stale_revision", Some("join_unavailable"))).unwrap_err();
        assert_eq!(join_unavailable().as_str(), from_bridge.as_str());
        assert!(matches!(
            (join_unavailable(), from_bridge),
            (Failure::Bridge(one), Failure::Bridge(other)) if one == other
        ));
    }

    #[tokio::test]
    async fn a_join_is_checked_and_asked_of_an_approved_bridge_only() {
        let dir = std::env::temp_dir().join(format!("sf4-net-friends-join-{}", std::process::id()));
        let shared = shared_pinning(&dir, &[]);
        let refused = |action: Value| {
            let shared = shared.clone();
            async move {
                act(&shared, "brg_never_approved", serde_json::from_value(action).unwrap())
                    .await
                    .unwrap_err()
                    .as_str()
                    .into_owned()
            }
        };
        assert_eq!(
            refused(json!({ "action": "join", "friend": REQUEST })).await,
            "invalid_request"
        );
        assert_eq!(
            refused(json!({ "action": "join", "friend": FRIEND })).await,
            "bridge_not_approved"
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    // A request a bridge refused as malformed, with a member only a newer
    // bridge reads, makes the next call read its capabilities again.
    #[test]
    fn only_a_malformed_refusal_counts_against_the_members_sent() {
        let malformed = reply(400, &refusal("invalid_request", None)).unwrap_err();
        assert!(refused_as_malformed(&malformed));
        assert!(refused_as_malformed(&reply(400, b"").unwrap_err()));
        // A reason the route words apart, a refusal of another kind and a
        // failing bridge are not.
        let own_card = reply(400, &refusal("invalid_request", Some("own_card"))).unwrap_err();
        assert_eq!(own_card.as_str(), "own_card");
        assert!(!refused_as_malformed(&own_card));
        let stale = reply(409, &refusal("stale_revision", None)).unwrap_err();
        assert!(!refused_as_malformed(&stale));
        assert!(!refused_as_malformed(&reply(503, b"").unwrap_err()));
        assert!(!refused_as_malformed(&Failure::new(Code::Internal)));
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
                    .into_owned()
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
                    .into_owned()
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
        for reason in FriendRefusal::NAMES {
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
    fn the_state_is_read_ignoring_members_it_does_not_know() {
        let state = json!({ "code": "4F2A-91CX", "friends": [], "incoming": [],
            "outgoing": [], "invites": [], "blocked": [] });
        let read = |value: &Value| -> Result<FriendsState, Failure> {
            client::read(200, &serde_json::to_vec(value).unwrap(), &[200], &REFUSALS)
        };
        assert!(read(&state).unwrap().check().is_ok());
        let mut with_extra = state.clone();
        with_extra["extra"] = json!(1);
        assert!(read(&with_extra).unwrap().check().is_ok());
        let mut without_code = state.clone();
        without_code.as_object_mut().unwrap().remove("code");
        let mut in_friend = state.clone();
        in_friend["friends"] = json!([{ "id": FRIEND, "name": "Player 3", "since": 1,
            "presence": { "state": "offline" }, "extra": 1 }]);
        assert!(read(&in_friend).unwrap().check().is_ok());
        // A friend left without its presence is not a state of any bridge.
        let mut without_presence = state.clone();
        without_presence["friends"] = json!([{ "id": FRIEND, "name": "Player 3", "since": 1 }]);
        for bad in [without_code, without_presence] {
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

    // A later bridge's status, activity, invite or badge is read as a neutral
    // one, or left out, and the rest of the list stays: the helper must not
    // refuse the whole state for one value it does not know.
    #[test]
    fn the_state_survives_one_value_it_does_not_know() {
        let room = "0f".repeat(16);
        let state = json!({ "code": "4F2A-91CX",
            "friends": [
                { "id": FRIEND, "name": "Player 3", "since": 1, "badge": { "band": 7, "settled": true, "later": 1 },
                  "presence": { "state": "room", "status": "arcade_queue" } },
                { "id": FRIEND, "name": "Player 4", "since": 1,
                  "presence": { "state": "online", "activity": "streaming" } },
                { "id": FRIEND, "name": "Player 5", "since": 1,
                  "presence": { "state": "spectating" } },
            ],
            "incoming": [], "outgoing": [],
            "invites": [
                { "id": "iv_11111111-1111-4111-8111-111111111111", "friend": FRIEND,
                  "room": { "lobby": "x" }, "expires_at": 9 },
                { "id": "iv_22222222-2222-4222-8222-222222222222", "friend": FRIEND,
                  "room": { "room_id": room }, "expires_at": 9 },
            ],
            "blocked": [] });
        let read: FriendsState =
            client::read(200, &serde_json::to_vec(&state).unwrap(), &[200], &REFUSALS).unwrap();
        assert!(read.check().is_ok());
        let wire = serde_json::to_value(&read).unwrap();
        assert_eq!(wire["friends"][0]["presence"], json!({ "state": "room", "status": "idle" }));
        assert!(wire["friends"][0].get("badge").is_none());
        assert_eq!(wire["friends"][1]["presence"], json!({ "state": "online", "activity": "menus" }));
        assert_eq!(wire["friends"][2]["presence"], json!({ "state": "online", "activity": "menus" }));
        assert_eq!(wire["invites"].as_array().unwrap().len(), 1);
    }

    // A newer bridge's game, private room and invitation reach the game as
    // sent, and a value of them this build cannot use drops only itself.
    #[test]
    fn the_presence_members_pass_through_to_the_game() {
        let secret = format!("emd1:{}", "A".repeat(122));
        let state = json!({ "code": "4F2A-91CX",
            "friends": [
                { "id": FRIEND, "name": "Player 3", "since": 1,
                  "presence": { "state": "room", "status": "idle", "private": true, "game": "usf4",
                      "private_room": { "name": "Lounge", "members": 2, "capacity": 8,
                                        "joinable": true } } },
                { "id": FRIEND, "name": "Player 4", "since": 1,
                  "presence": { "state": "room", "status": "idle", "private": true, "game": "sfv" } },
                { "id": FRIEND, "name": "Player 5", "since": 1,
                  "presence": { "state": "online", "activity": "menus", "game": "sfv" } },
                { "id": FRIEND, "name": "Player 6", "since": 1,
                  "presence": { "state": "room", "status": "idle", "private": true,
                      "game": "SFV", "private_room": { "name": "" } } },
            ],
            "incoming": [], "outgoing": [], "invites": [], "blocked": [],
            "joining": { "friend": FRIEND, "secret": secret } });
        let read: FriendsState =
            client::read(200, &serde_json::to_vec(&state).unwrap(), &[200], &REFUSALS).unwrap();
        assert!(read.check().is_ok());
        let wire = serde_json::to_value(&read).unwrap();
        for friend in 0..3 {
            assert_eq!(
                wire["friends"][friend]["presence"],
                state["friends"][friend]["presence"]
            );
        }
        assert_eq!(wire["joining"], state["joining"]);
        // A game or room this build cannot use is left out; the friend stays.
        assert_eq!(
            wire["friends"][3]["presence"],
            json!({ "state": "room", "status": "idle", "private": true })
        );
        // The invitation is not in anything a log line of the state prints.
        assert!(!format!("{read:?}").contains(&secret));
    }

    #[test]
    fn every_reason_the_bridge_writes_is_passed_on() {
        for reason in ["blocks_full", "friends_off", "no_discord_account", "join_unavailable"] {
            assert!(FriendRefusal::NAMES.contains(&reason), "{reason}");
            let body = refusal("stale_revision", Some(reason));
            assert_eq!(reply(409, &body).unwrap_err().as_str(), reason);
        }
    }
    // A friend's account points go to the game as the bridge sent them, and a
    // friend without them stays without.
    #[test]
    fn a_friends_account_points_pass_through_and_may_be_absent() {
        let state = |friend: Value| {
            json!({ "code": "4F2A-91CX", "friends": [friend], "incoming": [],
                "outgoing": [], "invites": [], "blocked": [] })
        };
        let friend = json!({ "id": FRIEND, "name": "Player 3", "since": 1,
            "account": { "points": 2331, "tier": 3 }, "presence": { "state": "offline" } });
        let read = |value: &Value| -> FriendsState {
            client::read(200, &serde_json::to_vec(value).unwrap(), &[200], &REFUSALS).unwrap()
        };
        let with = read(&state(friend.clone()));
        assert!(with.check().is_ok());
        assert_eq!(serde_json::to_value(&with).unwrap()["friends"][0]["account"], friend["account"]);
        let mut without = friend.clone();
        without.as_object_mut().unwrap().remove("account");
        let without = serde_json::to_value(read(&state(without))).unwrap();
        assert!(without["friends"][0].get("account").is_none());
        let mut bad = friend;
        bad["account"]["tier"] = json!(9);
        assert!(read(&state(bad)).check().is_err());
    }
}
