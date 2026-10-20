//! Watching a bridge match (docs/design/MATCH_WATCH.md): who the signed
//! binding names beside the two fighters, and the player route a watcher's
//! helper calls to ask for the match's room.
//!
//! A watcher gets no binding. The bridge answers it with the room and its
//! invitation once it may watch, and names it in the fighters' binding, which
//! is how the room learns to admit it.
use serde::{Deserialize, Serialize};

use super::{BoundFighter, check_helper, is_hex};
use crate::{EmberId, Error, Result};

/// The capabilities feature of a bridge that serves the watch routes, may sign
/// bindings that name watchers and reads a claim's `watch`.
pub const WATCH_FEATURE: &str = "matches.watch";
/// Watchers one binding names, staff included; the room's `MaxWatchers`.
pub const MAX_WATCHERS: usize = 8;

/// An endpoint the bridge admitted to watch the match, and who answers for it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoundWatcher {
    pub ember_id: EmberId,
    pub endpoint_id: String,
}

/// The binding's watchers: few, valid, and distinct from each other and from
/// the fighters, by Ember ID and by endpoint.
pub(super) fn check_watchers(
    fighters: &[BoundFighter; 2],
    watchers: &[BoundWatcher],
) -> Result<()> {
    if watchers.len() > MAX_WATCHERS || watchers.iter().any(|w| !is_hex(&w.endpoint_id, 64)) {
        return Err(Error::InvalidField("watchers"));
    }
    let seats = || {
        fighters
            .iter()
            .map(|f| (&f.ember_id, &f.endpoint_id))
            .chain(watchers.iter().map(|w| (&w.ember_id, &w.endpoint_id)))
    };
    for (at, (ember_id, endpoint_id)) in seats().enumerate() {
        if seats()
            .skip(at + 1)
            .any(|(other_id, other_endpoint)| other_id == ember_id || other_endpoint == endpoint_id)
        {
            return Err(Error::Mismatch("duplicate watcher"));
        }
    }
    Ok(())
}

/// `POST /v1/matches/{match_id}/watch` (`match.watch`): a player's helper asks
/// to watch, and asks again every few seconds to stay a watcher.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct WatchRequest {
    pub endpoint_id: String,
    pub helper_instance_id: String,
    pub build_id: String,
}

impl WatchRequest {
    pub fn check(&self) -> Result<()> {
        check_helper(&self.endpoint_id, &self.helper_instance_id, &self.build_id)
    }
}

/// Why a watcher is not in the room yet.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WaitReason {
    /// No room is published yet.
    NoRoom,
    /// The bridge has no binding that can name the watcher yet: a fighter has
    /// not claimed, or the fighters' helpers have not both said they read
    /// watchers (`Claim::watch`).
    NotBound,
    /// The binding has no place for this watcher now, and one may free up:
    /// the link-holder cap or the ceiling is reached, another watcher holds
    /// the same endpoint, or the endpoint is a fighter's own.
    Full,
}

/// What a watch request returns.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "state", rename_all = "snake_case", deny_unknown_fields)]
pub enum WatchAnswer {
    Waiting {
        retry_after: u64,
        reason: WaitReason,
    },
    /// The bound set names this watcher: join the room with the invitation.
    Room { room_id: String, invitation: String },
}

#[cfg(test)]
mod tests {
    use zeroize::Zeroizing;

    use super::*;
    use crate::{
        SigningIdentity,
        encoding::Counter,
        json,
        matches::Rules,
        play::{Binding, PROFILE, VERSION, roster_digest, rules_digest},
    };

    fn key(byte: u8) -> SigningIdentity {
        SigningIdentity::from_seed(&Zeroizing::new([byte; 32])).unwrap()
    }

    fn watcher(byte: u8) -> BoundWatcher {
        BoundWatcher {
            ember_id: key(byte).ember_id().clone(),
            endpoint_id: format!("{byte:02x}").repeat(32),
        }
    }

    fn binding(watchers: Vec<BoundWatcher>) -> Binding {
        let (a, b) = (key(1), key(2));
        Binding {
            version: VERSION,
            bridge_id: "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11".into(),
            match_id: "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12".into(),
            assignment_generation: Counter(1),
            binding_revision: Counter(1),
            room_id: "0123456789abcdef0123456789abcdef".into(),
            build_id: "build".into(),
            games_to_win: 2,
            rules_digest: rules_digest(&Rules::standard(2, PROFILE)).unwrap(),
            roster_digest: roster_digest(a.ember_id(), b.ember_id()).unwrap(),
            fighters: [
                BoundFighter {
                    ember_id: a.ember_id().clone(),
                    endpoint_id: "01".repeat(32),
                },
                BoundFighter {
                    ember_id: b.ember_id().clone(),
                    endpoint_id: "02".repeat(32),
                },
            ],
            issued_at: 100,
            starting_wins: [0, 0],
            watchers,
        }
    }

    /// The binding as every released helper reads it: no `watchers` field.
    #[derive(Deserialize)]
    #[serde(deny_unknown_fields)]
    #[allow(dead_code)]
    struct Released {
        version: u8,
        bridge_id: String,
        match_id: String,
        assignment_generation: Counter,
        binding_revision: Counter,
        room_id: String,
        build_id: String,
        games_to_win: u8,
        rules_digest: String,
        roster_digest: String,
        fighters: [BoundFighter; 2],
        issued_at: u64,
    }

    #[test]
    fn a_binding_without_watchers_is_the_bytes_every_released_helper_accepts() {
        let bare = binding(vec![]);
        let value = serde_json::to_value(&bare).unwrap();
        assert!(value.get("watchers").is_none(), "{value}");
        serde_json::from_value::<Released>(value.clone()).unwrap();
        // Read back and signed again, it is the same bytes.
        let read: Binding = serde_json::from_value(value).unwrap();
        assert_eq!(
            json::to_value(&read).unwrap(),
            json::to_value(&bare).unwrap()
        );
    }

    #[test]
    fn a_binding_with_watchers_signs_and_verifies_and_a_released_helper_refuses_it() {
        let bridge = key(9);
        let named = binding(vec![watcher(3), watcher(4)]);
        let signed = named.sign(&bridge, "k1").unwrap();
        assert_eq!(signed.verify(&bridge.public_key(), "k1").unwrap(), &named);
        let value = serde_json::to_value(&named).unwrap();
        assert_eq!(value["watchers"].as_array().unwrap().len(), 2);
        assert!(serde_json::from_value::<Released>(value).is_err());
        // The watchers are signed: dropping one fails verification.
        let mut tampered = signed.clone();
        tampered.binding.watchers.pop();
        assert!(tampered.verify(&bridge.public_key(), "k1").is_err());
    }

    #[test]
    fn a_binding_names_few_distinct_valid_watchers() {
        let check = |watchers| binding(watchers).check();
        assert!(check((3..3 + MAX_WATCHERS as u8).map(watcher).collect()).is_ok());
        assert_eq!(
            check((3..4 + MAX_WATCHERS as u8).map(watcher).collect()),
            Err(Error::InvalidField("watchers"))
        );
        let mut bad_hex = watcher(3);
        bad_hex.endpoint_id = "A".repeat(64);
        assert_eq!(check(vec![bad_hex]), Err(Error::InvalidField("watchers")));
        let duplicate = Err(Error::Mismatch("duplicate watcher"));
        assert_eq!(check(vec![watcher(3), watcher(3)]), duplicate);
        // The same Ember ID on another endpoint, and another Ember ID on the
        // same endpoint, are both duplicates.
        let mut same_id = watcher(3);
        same_id.endpoint_id = "04".repeat(32);
        assert_eq!(check(vec![watcher(3), same_id]), duplicate);
        let mut same_endpoint = watcher(4);
        same_endpoint.endpoint_id = watcher(3).endpoint_id;
        assert_eq!(check(vec![watcher(3), same_endpoint]), duplicate);
        // A watcher is not a fighter, by Ember ID or by endpoint.
        let mut fighter_id = watcher(3);
        fighter_id.ember_id = key(1).ember_id().clone();
        assert_eq!(check(vec![fighter_id]), duplicate);
        let mut fighter_endpoint = watcher(3);
        fighter_endpoint.endpoint_id = "02".repeat(32);
        assert_eq!(check(vec![fighter_endpoint]), duplicate);
    }

    #[test]
    fn the_watch_command_parses_strictly() {
        let command = serde_json::json!({
            "endpoint_id": "a".repeat(64),
            "helper_instance_id": "ins_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a01",
            "build_id": "build",
        });
        let request: WatchRequest = serde_json::from_value(command.clone()).unwrap();
        assert!(request.check().is_ok());
        assert_eq!(serde_json::to_value(&request).unwrap(), command);
        for (field, value) in [
            ("endpoint_id", "a".repeat(63)),
            ("helper_instance_id", "ins_x".into()),
            ("build_id", String::new()),
        ] {
            let mut bad = command.clone();
            bad[field] = value.into();
            let parsed: WatchRequest = serde_json::from_value(bad).unwrap();
            assert_eq!(parsed.check(), Err(Error::InvalidField(field)));
        }
        let mut unknown = command;
        unknown["ready"] = true.into();
        assert!(serde_json::from_value::<WatchRequest>(unknown).is_err());
    }

    #[test]
    fn the_watch_answer_parses_strictly() {
        let waiting =
            serde_json::json!({ "state": "waiting", "retry_after": 10, "reason": "not_bound" });
        assert_eq!(
            serde_json::from_value::<WatchAnswer>(waiting.clone()).unwrap(),
            WatchAnswer::Waiting {
                retry_after: 10,
                reason: WaitReason::NotBound
            }
        );
        let room = serde_json::json!({ "state": "room", "room_id": "f".repeat(32), "invitation": "sf4e3:abc" });
        let parsed: WatchAnswer = serde_json::from_value(room.clone()).unwrap();
        assert_eq!(serde_json::to_value(&parsed).unwrap(), room);
        let mut extra = waiting;
        extra["binding"] = serde_json::Value::Null;
        assert!(serde_json::from_value::<WatchAnswer>(extra).is_err());
        for reason in ["no_room", "not_bound", "full"] {
            let text = format!(r#"{{"state":"waiting","retry_after":1,"reason":"{reason}"}}"#);
            assert!(json::parse_as::<WatchAnswer>(text.as_bytes(), 1024).is_ok());
        }
        let unknown = br#"{"state":"waiting","retry_after":1,"reason":"x"}"#;
        assert!(json::parse_as::<WatchAnswer>(unknown, 1024).is_err());
    }
}
