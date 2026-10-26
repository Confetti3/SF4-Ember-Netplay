//! Watching a bridge match for the native side (docs/design/MATCH_WATCH.md
//! 5.3). A watcher's helper asks the match's bridge for the room and its
//! invitation, again every few seconds to stay a watcher. It gets no binding:
//! its game never applies one, and the room learns of the watcher from the
//! fighters' binding instead. Only a bridge that lists the watch feature is
//! asked, so an older one never sees the route.
use std::sync::Arc;

use ember_protocol::{
    challenge::Action,
    play::{WatchAnswer, WatchRequest},
};
use serde_json::{Value, json};

use super::{
    Failure, Outcome, Shared,
    bridges::Approved,
    client::{self, answer, refusal},
    play::{Need, check_match_id, features, send_proven},
};

/// The refusals the game words apart, from the bridge's `details.reason`:
/// the match is over, the caller plays in it, the organizer has not opened it
/// to the caller, or has denied the caller this match.
const REASONS: [&str; 4] = ["finished", "fighter", "not_allowed", "revoked"];

/// Asks to watch `match_id` with this helper run's endpoint. The answer is
/// the room to join, or why to wait and for how long.
pub async fn watch_match(
    shared: &Arc<Shared>,
    bridge_id: &str,
    match_id: &str,
    build: &str,
) -> Outcome {
    check_match_id(match_id)?;
    let request = WatchRequest {
        endpoint_id: shared.endpoint_id.clone(),
        helper_instance_id: shared.instance_id.clone(),
        build_id: build.to_owned(),
    };
    request
        .check()
        .map_err(|_| Failure::new("invalid_request"))?;
    let command = serde_json::to_value(&request).map_err(|_| Failure::new("internal"))?;
    if !serves_watch(shared, &client::approved(shared, bridge_id)?).await? {
        return Err(Failure::new("watch_unsupported"));
    }
    client::with_session(shared, bridge_id, |bridge, token| {
        let command = command.clone();
        async move {
            let path = format!("/v1/matches/{match_id}/watch");
            let (status, body) =
                send_proven(shared, &bridge, &token, Action::MatchWatch, &path, command).await?;
            present(reply(status, &body)?).map(Some)
        }
    })
    .await
}

/// Stops watching `match_id`, so the bridge drops this watcher at once
/// rather than when its polls stop. A bridge that does not serve watching
/// was never asked to watch, so there is nothing to stop and it is not asked.
pub async fn unwatch_match(shared: &Arc<Shared>, bridge_id: &str, match_id: &str) -> Outcome {
    check_match_id(match_id)?;
    if !serves_watch(shared, &client::approved(shared, bridge_id)?).await? {
        return Ok(Some(json!({})));
    }
    client::with_session(shared, bridge_id, |bridge, token| async move {
        let path = format!("/v1/matches/{match_id}/watch/leave");
        let (status, body) = send_proven(
            shared,
            &bridge,
            &token,
            Action::MatchUnwatch,
            &path,
            json!({}),
        )
        .await?;
        reply(status, &body)?;
        Ok(Some(json!({})))
    })
    .await
}

/// Whether the bridge serves the watch routes, from the capabilities this run
/// keeps for it (`play::features`). One that does not is asked again next
/// time, since it may be upgraded meanwhile.
async fn serves_watch(shared: &Arc<Shared>, bridge: &Approved) -> Result<bool, Failure> {
    Ok(features(shared, bridge, Need::Watch).await?.watch)
}

/// The bridge's answer as JSON when it is a success. A refusal is the
/// bridge's own reason when it names one the game words, and its error code
/// otherwise.
fn reply(status: u16, body: &[u8]) -> Result<Value, Failure> {
    if status == 200 {
        return answer(status, body, &[200]);
    }
    Err(refusal(status, body, &REASONS))
}

/// The watch answer for the game, in the claim answer's `role` shape.
fn present(value: Value) -> Result<Value, Failure> {
    let invalid = || Failure::new("bridge_invalid_response");
    match serde_json::from_value::<WatchAnswer>(value).map_err(|_| invalid())? {
        WatchAnswer::Waiting {
            retry_after,
            reason,
        } => Ok(json!({ "role": "waiting", "retry_after": retry_after, "reason": reason })),
        WatchAnswer::Room {
            room_id,
            invitation,
        } => {
            if !ember_protocol::play::is_hex(&room_id, 32) || invitation.is_empty() {
                return Err(invalid());
            }
            Ok(json!({ "role": "room", "room_id": room_id, "invitation": invitation }))
        }
    }
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicU8, Ordering};

    use super::*;
    use crate::{
        identity::Protection,
        tournament::{
            Request,
            builtin::Builtin,
            play::send_claim,
            test_support::{capabilities_server, shared_pinning},
        },
    };

    fn refusal(status: u16, code: &str, reason: Option<&str>) -> Result<Value, Failure> {
        let details = reason.map_or(json!({}), |reason| json!({ "reason": reason }));
        let body = json!({ "error": {
            "code": code, "message": "m", "retryable": false, "request_id": "req_1", "details": details,
        }});
        reply(status, &serde_json::to_vec(&body).unwrap())
    }

    #[test]
    fn a_refusal_names_the_bridges_reason_or_its_code() {
        for reason in REASONS {
            assert_eq!(
                refusal(403, "forbidden", Some(reason)).unwrap_err().0,
                reason
            );
        }
        // A reason the game does not word falls back to the error code.
        assert_eq!(
            refusal(403, "forbidden", Some("other")).unwrap_err().0,
            "forbidden"
        );
        assert_eq!(
            refusal(422, "incompatible_build", None).unwrap_err().0,
            "incompatible_build"
        );
        assert_eq!(
            reply(200, br#"{"left":true}"#).unwrap(),
            json!({ "left": true })
        );
    }

    #[test]
    fn the_game_gets_the_room_or_why_to_wait() {
        let room =
            present(json!({ "state": "room", "room_id": "a".repeat(32), "invitation": "sf4e3:x" }));
        assert_eq!(
            room.unwrap(),
            json!({ "role": "room", "room_id": "a".repeat(32), "invitation": "sf4e3:x" })
        );
        let waiting = present(json!({ "state": "waiting", "retry_after": 10, "reason": "full" }));
        assert_eq!(
            waiting.unwrap(),
            json!({ "role": "waiting", "retry_after": 10, "reason": "full" })
        );
        // A field a newer bridge adds is ignored; the game sees the answer
        // without it.
        let extra = present(
            json!({ "state": "room", "room_id": "a".repeat(32), "invitation": "x", "binding": null }),
        );
        assert_eq!(
            extra.unwrap(),
            json!({ "role": "room", "room_id": "a".repeat(32), "invitation": "x" })
        );
        // A known field of the wrong type, or a value no bridge sends, is
        // still refused.
        for bad in [
            json!({ "state": "room", "room_id": "A".repeat(32), "invitation": "sf4e3:x" }),
            json!({ "state": "room", "room_id": "a".repeat(32), "invitation": "" }),
            json!({ "state": "waiting", "retry_after": 10, "reason": "later" }),
            json!({ "state": "room", "room_id": 7, "invitation": "sf4e3:x" }),
            json!({ "state": "waiting", "retry_after": "10", "reason": "full" }),
            json!({ "state": "waiting", "retry_after": -1, "reason": "full" }),
        ] {
            assert_eq!(present(bad).unwrap_err().0, "bridge_invalid_response");
        }
    }

    #[test]
    fn the_game_asks_to_watch_and_to_stop() {
        let watch = json!({ "op": "match_watch", "bridge_id": "brg_x", "match_id": "emt_x", "build": "b1" });
        assert!(matches!(
            serde_json::from_value::<Request>(watch.clone()).unwrap(),
            Request::MatchWatch { .. }
        ));
        let mut extra = watch;
        extra["ready"] = true.into();
        assert!(serde_json::from_value::<Request>(extra).is_err());
        let unwatch = json!({ "op": "match_unwatch", "bridge_id": "brg_x", "match_id": "emt_x" });
        assert!(matches!(
            serde_json::from_value::<Request>(unwatch).unwrap(),
            Request::MatchUnwatch { .. }
        ));
    }

    // A bridge that does not list the feature is asked nothing past its
    // capabilities, to watch or to stop, and is asked again next time. One
    // that lists it is remembered, and the request goes on to the session it
    // needs.
    #[tokio::test]
    async fn a_bridge_without_the_watch_feature_is_sent_nothing() {
        let mut seed = [0; 16];
        getrandom::fill(&mut seed).unwrap();
        let dir =
            std::env::temp_dir().join(ember_protocol::encoding::prefixed_id("sf4-net-watch", seed));
        let mode = Arc::new(AtomicU8::new(1));
        let (origin, asked) = capabilities_server(mode.clone()).await;
        let key = ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([7; 32]))
            .unwrap()
            .public_key()
            .to_b64u();
        let pin = Builtin {
            bridge_id: ember_protocol::encoding::prefixed_id("brg", [7; 16]),
            origin,
            display_name: "Bridge".into(),
            keys: vec![("k7".into(), key)],
        };
        let shared = shared_pinning(&dir, std::slice::from_ref(&pin));
        shared
            .identity
            .lock()
            .unwrap()
            .enable(Protection::Passphrase("a test passphrase"))
            .unwrap();
        let count = || asked.load(Ordering::SeqCst);
        let match_id = ember_protocol::encoding::prefixed_id("emt", [2; 16]);
        let watch = || watch_match(&shared, &pin.bridge_id, &match_id, "b1");
        let unwatch = || unwatch_match(&shared, &pin.bridge_id, &match_id);

        assert_eq!(watch().await.unwrap_err().0, "watch_unsupported");
        assert_eq!(watch().await.unwrap_err().0, "watch_unsupported");
        assert_eq!(count(), 2, "only the capabilities were read");
        // Nothing to stop, and no session opened to say so.
        assert_eq!(unwatch().await.unwrap(), Some(json!({})));
        assert_eq!(count(), 3, "only the capabilities were read");

        mode.store(2, Ordering::SeqCst);
        assert_ne!(watch().await.unwrap_err().0, "watch_unsupported");
        let after = count();
        assert!(after > 4, "the watch went on to open a session");
        mode.store(1, Ordering::SeqCst);
        assert_ne!(watch().await.unwrap_err().0, "watch_unsupported");
        // The bridge that was seen to serve it is not asked again to stop.
        let before = count();
        let _ = unwatch().await;
        assert!(count() > before, "the stop went on to open a session");
        let _ = std::fs::remove_dir_all(dir);
    }

    // A fighter's claim says its helper reads watchers to a bridge that
    // serves watching, and to no other: an older bridge refuses the field. The
    // bridge is asked once, and again after a claim to it fails, which is how
    // an upgrade or a rollback shows.
    #[tokio::test]
    async fn a_claim_says_it_reads_watchers_only_to_a_bridge_that_serves_watching() {
        let dir = std::env::temp_dir().join(ember_protocol::encoding::prefixed_id(
            "sf4-net-claim-watch",
            [3; 16],
        ));
        let shared = shared_pinning(&dir, &[]);
        let mode = Arc::new(AtomicU8::new(1));
        let (origin, asked) = capabilities_server(mode.clone()).await;
        let bridge = Approved {
            bridge_id: "brg_x".into(),
            origin,
            display_name: "Bridge".into(),
            approved_at: 0,
            keys: Vec::new(),
        };
        let claim = ember_protocol::play::Claim {
            endpoint_id: "a".repeat(64),
            helper_instance_id: shared.instance_id.clone(),
            build_id: "b1".into(),
            ready: None,
            watch: None,
            held_binding: None,
        };
        let sent = std::sync::Mutex::new(Vec::new());
        let send = |fails: bool| {
            let sent = &sent;
            move |command: Value| async move {
                sent.lock().unwrap().push(command.get("watch").cloned());
                if fails {
                    Err(Failure::new("bridge_unavailable"))
                } else {
                    Ok(json!({}))
                }
            }
        };
        let claimed = |fails: bool| send_claim(&shared, &bridge, claim.clone(), false, send(fails));
        let count = || asked.load(Ordering::SeqCst);
        let set = |value| mode.store(value, Ordering::SeqCst);

        // Not served: the claims say nothing, and ask once.
        assert!(claimed(false).await.is_ok());
        assert!(claimed(false).await.is_ok());
        assert_eq!(count(), 1);
        // Upgraded: not heard until a claim fails, as one does while the
        // bridge restarts; the next then asks and says it.
        set(2);
        assert!(claimed(false).await.is_ok());
        assert_eq!(count(), 1);
        assert!(claimed(true).await.is_err());
        assert!(claimed(false).await.is_ok());
        assert!(claimed(false).await.is_ok());
        assert_eq!(count(), 2);
        // A claim saying it that fails forgets, and a capabilities read that
        // fails fails the claim rather than sending one that drops the
        // watchers.
        assert!(claimed(true).await.is_err());
        set(0);
        assert!(claimed(false).await.is_err());
        set(2);
        assert!(claimed(false).await.is_ok());
        assert_eq!(count(), 4);
        let yes = Some(json!(true));
        assert_eq!(
            *sent.lock().unwrap(),
            vec![
                None,
                None,
                None,
                None,
                yes.clone(),
                yes.clone(),
                yes.clone(),
                yes
            ]
        );
        let _ = std::fs::remove_dir_all(dir);
    }
}
