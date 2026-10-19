//! Watching a bridge match for the native side (docs/design/MATCH_WATCH.md
//! 5.3). A watcher's helper asks the match's bridge for the room and its
//! invitation, again every few seconds to stay a watcher. It gets no binding:
//! its game never applies one, and the room learns of the watcher from the
//! fighters' binding instead. Only a bridge that lists the watch feature is
//! asked, so an older one never sees the route.
use std::sync::Arc;

use ember_protocol::{
    api::ApiError,
    challenge::Action,
    play::{WATCH_FEATURE, WatchAnswer, WatchRequest},
};
use serde_json::{Value, json};

use super::{
    Failure, Outcome, Shared,
    bridges::Approved,
    client::{self, answer},
    play::{check_match_id, play_state, send_proven},
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
/// rather than when its polls stop.
pub async fn unwatch_match(shared: &Arc<Shared>, bridge_id: &str, match_id: &str) -> Outcome {
    check_match_id(match_id)?;
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

/// Whether the bridge serves the watch routes. One that does is remembered
/// for the rest of the run; one that does not is asked again next time, since
/// it may be upgraded meanwhile.
async fn serves_watch(shared: &Arc<Shared>, bridge: &Approved) -> Result<bool, Failure> {
    if play_state(shared)?
        .watch_bridges
        .contains(&bridge.bridge_id)
    {
        return Ok(true);
    }
    let serves = shared
        .client
        .capabilities(&bridge.origin)
        .await?
        .capabilities
        .features
        .iter()
        .any(|feature| feature == WATCH_FEATURE);
    if serves {
        play_state(shared)?
            .watch_bridges
            .insert(bridge.bridge_id.clone());
    }
    Ok(serves)
}

/// Whether a fighter's claim for `match_id` says this helper reads a binding
/// that names watchers: whenever the bridge serves watching, since the bridge
/// names watchers only while both fighters' claims say so. A bridge that does
/// not is not asked again for the match. A capabilities read that fails fails
/// the claim, which the game retries, rather than sending one without it,
/// which would take the match's watchers out of its binding.
pub(super) async fn claims_watch(
    shared: &Arc<Shared>,
    bridge: &Approved,
    match_id: &str,
) -> Result<bool, Failure> {
    if play_state(shared)?.plain_claims.contains(match_id) {
        return Ok(false);
    }
    let serves = serves_watch(shared, bridge).await?;
    if !serves {
        play_state(shared)?.plain_claims.insert(match_id.to_owned());
    }
    Ok(serves)
}

/// Forgets that the bridge serves watching, after a claim saying the helper
/// reads watchers failed: the bridge may have been put back to one that
/// refuses the field, so the next claim asks its capabilities again.
pub(super) fn unlearn_watch(shared: &Shared, bridge: &Approved) -> Result<(), Failure> {
    play_state(shared)?.watch_bridges.remove(&bridge.bridge_id);
    Ok(())
}

/// The bridge's answer as JSON when it is a success. A refusal is the
/// bridge's own reason when it names one the game words, and its error code
/// otherwise.
fn reply(status: u16, body: &[u8]) -> Result<Value, Failure> {
    if status == 200 {
        return answer(status, body, &[200]);
    }
    let reason = serde_json::from_slice::<ApiError>(body)
        .ok()
        .and_then(|error| {
            error
                .error
                .details
                .get("reason")?
                .as_str()
                .map(str::to_owned)
        })
        .filter(|reason| REASONS.contains(&reason.as_str()));
    Err(reason.map_or_else(|| answer(status, body, &[]).unwrap_err(), Failure))
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
            play::{
                send_claim,
                tests::{capabilities_server, shared_pinning},
            },
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
        for bad in [
            json!({ "state": "room", "room_id": "A".repeat(32), "invitation": "sf4e3:x" }),
            json!({ "state": "room", "room_id": "a".repeat(32), "invitation": "" }),
            json!({ "state": "waiting", "retry_after": 10, "reason": "later" }),
            json!({ "state": "room", "room_id": "a".repeat(32), "invitation": "x", "binding": null }),
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
    // capabilities, and is asked again next time. One that lists it is
    // remembered, and the watch request goes on to the session it needs.
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
            .enable(Protection::Default)
            .unwrap();
        let count = || asked.load(Ordering::SeqCst);
        let match_id = ember_protocol::encoding::prefixed_id("emt", [2; 16]);
        let watch = || watch_match(&shared, &pin.bridge_id, &match_id, "b1");

        assert_eq!(watch().await.unwrap_err().0, "watch_unsupported");
        assert_eq!(watch().await.unwrap_err().0, "watch_unsupported");
        assert_eq!(count(), 2, "only the capabilities were read");

        mode.store(2, Ordering::SeqCst);
        assert_ne!(watch().await.unwrap_err().0, "watch_unsupported");
        let after = count();
        assert!(after > 3, "the watch went on to open a session");
        mode.store(1, Ordering::SeqCst);
        assert_ne!(watch().await.unwrap_err().0, "watch_unsupported");
        let _ = std::fs::remove_dir_all(dir);
    }

    // A fighter's claim says its helper reads watchers to a bridge that
    // serves watching, and to no other: an older bridge refuses the field.
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
        let claimed = |match_id: &'static str, fails: bool| {
            send_claim(
                &shared,
                &bridge,
                match_id,
                claim.clone(),
                false,
                send(fails),
            )
        };
        let count = || asked.load(Ordering::SeqCst);
        let set = |value| mode.store(value, Ordering::SeqCst);

        // Not served: the match's claims say nothing, and ask once.
        assert!(claimed("emt_a", false).await.is_ok());
        assert!(claimed("emt_a", false).await.is_ok());
        assert_eq!(count(), 1);
        // Upgraded: that match stays as it was; a new one says it, asking once.
        set(2);
        assert!(claimed("emt_a", false).await.is_ok());
        assert!(claimed("emt_b", false).await.is_ok());
        assert!(claimed("emt_b", false).await.is_ok());
        assert_eq!(count(), 2);
        // A claim saying it that fails forgets, and the next asks again: a
        // capabilities read that fails fails the claim rather than sending
        // one that drops the watchers.
        assert!(claimed("emt_b", true).await.is_err());
        set(0);
        assert!(claimed("emt_b", false).await.is_err());
        set(2);
        assert!(claimed("emt_b", false).await.is_ok());
        assert_eq!(count(), 4);
        let yes = Some(json!(true));
        assert_eq!(
            *sent.lock().unwrap(),
            vec![None, None, None, yes.clone(), yes.clone(), yes.clone(), yes]
        );
        let _ = std::fs::remove_dir_all(dir);
    }
}
