//! Bridge-run tournament play (spec 14 to 16): a fighter claiming a match, the
//! room binding a bridge signs once both fighters have claimed, and the permit
//! that makes one native game an official attempt.
//!
//! The bridge signs bindings and permits with its own key under their own
//! domains, using the same canonical JSON and Ed25519 rules as player proofs,
//! instead of JWS (a recorded deviation from spec 9.4). Only a fighter's own
//! helper verifies them, against the key of a bridge the player approved; the
//! game receives the checked fields, never a token to judge.
use serde::{Deserialize, Serialize};

use crate::{
    EmberId, Error, PublicKey, Result, SigningIdentity,
    encoding::{Counter, decode_b64u, is_prefixed_id},
    json,
    matches::Rules,
    report::{GameReport, Outcome},
    sign::Domain,
};

/// The rules profile of matches whose games the fighters report: the room's
/// own game settings, results by two-player agreement or organizer review.
pub const PROFILE: &str = "ember-room-v1";
pub const VERSION: u8 = 1;
/// How long a permit may wait for its game to start (spec 15.3).
pub const PERMIT_START_SECS: u64 = 120;
/// A provisioning lease, renewed by claiming again (spec 14.1).
pub const LEASE_SECS: u64 = 30;
/// The room table a bound match plays on (spec 13.4).
pub const TABLE: u8 = 0;
pub const MAX_INVITATION: usize = 4096;
const MAX_BUILD: usize = 128;

/// `len` lowercase hex digits: room IDs (32) and Iroh endpoint IDs (64).
pub fn is_hex(text: &str, len: usize) -> bool {
    text.len() == len
        && text
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn check_build(build_id: &str) -> Result<()> {
    if build_id.is_empty() || build_id.len() > MAX_BUILD || !build_id.is_ascii() {
        return Err(Error::InvalidField("build_id"));
    }
    Ok(())
}

/// The rules every binding, permit and report of a match carries.
pub fn rules_digest(rules: &Rules) -> Result<String> {
    json::digest(rules)
}

/// The fighters by slot, P1 first.
pub fn roster_digest(p1: &EmberId, p2: &EmberId) -> Result<String> {
    json::digest(&[p1, p2])
}

/// `match.claim`: an assigned fighter offers this helper run's endpoint.
/// Claiming again renews the claim and any provisioning lease it holds.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Claim {
    pub endpoint_id: String,
    pub helper_instance_id: String,
    pub build_id: String,
}

impl Claim {
    pub fn check(&self) -> Result<()> {
        if !is_hex(&self.endpoint_id, 64) {
            return Err(Error::InvalidField("endpoint_id"));
        }
        if !is_prefixed_id(&self.helper_instance_id, "ins") {
            return Err(Error::InvalidField("helper_instance_id"));
        }
        check_build(&self.build_id)
    }
}

/// What a claim returns.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "role", rename_all = "snake_case", deny_unknown_fields)]
pub enum ClaimAnswer {
    /// No room yet and this fighter holds the provisioning lease: host one
    /// and publish it with this lease before it expires.
    Host {
        lease_id: String,
        fence: Counter,
        expires_at: u64,
    },
    /// The other fighter is setting up the room.
    Wait { retry_after: u64 },
    /// The match's room. The binding is present once both fighters have
    /// claimed with the endpoints it names.
    Room {
        room_id: String,
        invitation: String,
        binding: Option<Box<SignedBinding>>,
    },
}

/// `room.publish`: the room a fighter hosts for the match. A first room needs
/// the provisioning lease; replacing a room names the one it replaces, so two
/// fighters cannot both replace it; neither refreshes the current room's
/// invitation, which the room renews as it ages.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PublishRoom {
    pub room_id: String,
    pub invitation: String,
    pub lease_id: Option<String>,
    pub fence: Option<Counter>,
    pub replaces: Option<String>,
}

impl PublishRoom {
    pub fn check(&self) -> Result<()> {
        if !is_hex(&self.room_id, 32) {
            return Err(Error::InvalidField("room_id"));
        }
        if self.invitation.is_empty()
            || self.invitation.len() > MAX_INVITATION
            || !self.invitation.is_ascii()
        {
            return Err(Error::InvalidField("invitation"));
        }
        let leased = self.lease_id.is_some();
        let lease_valid = leased == self.fence.is_some()
            && self
                .lease_id
                .as_deref()
                .is_none_or(|id| is_prefixed_id(id, "lse"));
        if !lease_valid || (leased && self.replaces.is_some()) {
            return Err(Error::InvalidField("lease_id"));
        }
        if self.replaces.as_deref().is_some_and(|id| !is_hex(id, 32)) {
            return Err(Error::InvalidField("replaces"));
        }
        Ok(())
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoundFighter {
    pub ember_id: EmberId,
    pub endpoint_id: String,
}

/// Which two endpoints may sit at the match's table in which room, signed by
/// the bridge. `binding_revision` moves when a fighter's endpoint changes;
/// `assignment_generation` when the roster, rules or room are replaced.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Binding {
    pub version: u8,
    pub bridge_id: String,
    pub match_id: String,
    pub assignment_generation: Counter,
    pub binding_revision: Counter,
    pub room_id: String,
    pub build_id: String,
    pub games_to_win: u8,
    pub rules_digest: String,
    pub roster_digest: String,
    /// By slot: P1, then P2.
    pub fighters: [BoundFighter; 2],
    pub issued_at: u64,
}

impl Binding {
    pub fn check(&self) -> Result<()> {
        let fields = [
            (self.version == VERSION, "version"),
            (is_prefixed_id(&self.bridge_id, "brg"), "bridge_id"),
            (is_prefixed_id(&self.match_id, "emt"), "match_id"),
            (
                self.assignment_generation.is_positive(),
                "assignment_generation",
            ),
            (self.binding_revision.is_positive(), "binding_revision"),
            (is_hex(&self.room_id, 32), "room_id"),
            (matches!(self.games_to_win, 1 | 2 | 3 | 5), "games_to_win"),
            (
                self.fighters.iter().all(|f| is_hex(&f.endpoint_id, 64)),
                "fighters",
            ),
        ];
        if let Some((_, field)) = fields.iter().find(|(valid, _)| !valid) {
            return Err(Error::InvalidField(field));
        }
        check_build(&self.build_id)?;
        decode_b64u::<32>(&self.rules_digest, "rules_digest")?;
        decode_b64u::<32>(&self.roster_digest, "roster_digest")?;
        let [p1, p2] = &self.fighters;
        if p1.ember_id == p2.ember_id || p1.endpoint_id == p2.endpoint_id {
            return Err(Error::Mismatch("duplicate fighter"));
        }
        if roster_digest(&p1.ember_id, &p2.ember_id)? != self.roster_digest {
            return Err(Error::Mismatch("roster_digest"));
        }
        Ok(())
    }

    pub fn sign(&self, key: &SigningIdentity, kid: &str) -> Result<SignedBinding> {
        self.check()?;
        Ok(SignedBinding {
            binding: self.clone(),
            kid: kid.to_owned(),
            signature: key.sign(Domain::Binding, &json::to_value(self)?),
        })
    }

    /// The slot `endpoint_id` holds, if it is one of the two fighters.
    pub fn slot_of(&self, endpoint_id: &str) -> Option<u8> {
        (0..2u8).find(|&slot| self.fighters[slot as usize].endpoint_id == endpoint_id)
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SignedBinding {
    pub binding: Binding,
    pub kid: String,
    pub signature: String,
}

impl SignedBinding {
    /// Checks the signature against the trusted bridge key and the binding's
    /// own consistency. Whether it is the expected bridge and match is the
    /// caller's check.
    pub fn verify(&self, key: &PublicKey, kid: &str) -> Result<&Binding> {
        if self.kid != kid {
            return Err(Error::Mismatch("kid"));
        }
        self.binding.check()?;
        key.verify(
            Domain::Binding,
            &json::to_value(&self.binding)?,
            &self.signature,
        )?;
        Ok(&self.binding)
    }
}

/// `attempt.prepare`: one fighter's description of the game it is about to
/// start. The bridge issues a permit once both fighters describe the same one.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Prepare {
    pub assignment_generation: Counter,
    pub binding_revision: Counter,
    pub room_id: String,
    pub table_id: u8,
    pub match_generation: Counter,
    pub rules_digest: String,
    pub roster_digest: String,
    pub build_id: String,
    pub endpoint_id: String,
}

impl Prepare {
    pub fn check(&self) -> Result<()> {
        let fields = [
            (
                self.assignment_generation.is_positive(),
                "assignment_generation",
            ),
            (self.binding_revision.is_positive(), "binding_revision"),
            (is_hex(&self.room_id, 32), "room_id"),
            (self.table_id == TABLE, "table_id"),
            (self.match_generation.is_positive(), "match_generation"),
            (is_hex(&self.endpoint_id, 64), "endpoint_id"),
        ];
        if let Some((_, field)) = fields.iter().find(|(valid, _)| !valid) {
            return Err(Error::InvalidField(field));
        }
        check_build(&self.build_id)?;
        decode_b64u::<32>(&self.rules_digest, "rules_digest")?;
        decode_b64u::<32>(&self.roster_digest, "roster_digest")?;
        Ok(())
    }

    /// The fields both fighters must agree on (the endpoint is each one's own).
    pub fn same_game(&self, other: &Prepare) -> bool {
        self.assignment_generation == other.assignment_generation
            && self.binding_revision == other.binding_revision
            && self.room_id == other.room_id
            && self.table_id == other.table_id
            && self.match_generation == other.match_generation
            && self.rules_digest == other.rules_digest
            && self.roster_digest == other.roster_digest
            && self.build_id == other.build_id
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "state", rename_all = "snake_case", deny_unknown_fields)]
pub enum PrepareAnswer {
    /// Waiting for the other fighter's matching description.
    Pending {
        retry_after: u64,
    },
    Permitted {
        permit: Box<SignedPermit>,
    },
}

/// One official attempt: which native game in which room counts for the
/// match. It may start until `start_by`; a game that started in time is
/// reported against it however long it runs.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Permit {
    pub version: u8,
    pub bridge_id: String,
    pub match_id: String,
    pub assignment_generation: Counter,
    pub attempt_id: String,
    pub permit_id: String,
    pub room_id: String,
    pub table_id: u8,
    pub match_generation: Counter,
    pub p1_id: EmberId,
    pub p2_id: EmberId,
    pub rules_digest: String,
    pub roster_digest: String,
    pub build_id: String,
    pub issued_at: u64,
    pub start_by: u64,
}

impl Permit {
    pub fn check(&self) -> Result<()> {
        let fields = [
            (self.version == VERSION, "version"),
            (is_prefixed_id(&self.bridge_id, "brg"), "bridge_id"),
            (is_prefixed_id(&self.match_id, "emt"), "match_id"),
            (is_prefixed_id(&self.attempt_id, "ega"), "attempt_id"),
            (is_prefixed_id(&self.permit_id, "per"), "permit_id"),
            (
                self.assignment_generation.is_positive(),
                "assignment_generation",
            ),
            (is_hex(&self.room_id, 32), "room_id"),
            (self.table_id == TABLE, "table_id"),
            (self.match_generation.is_positive(), "match_generation"),
            (self.start_by > self.issued_at, "start_by"),
        ];
        if let Some((_, field)) = fields.iter().find(|(valid, _)| !valid) {
            return Err(Error::InvalidField(field));
        }
        check_build(&self.build_id)?;
        decode_b64u::<32>(&self.rules_digest, "rules_digest")?;
        if self.p1_id == self.p2_id {
            return Err(Error::Mismatch("duplicate fighter"));
        }
        if roster_digest(&self.p1_id, &self.p2_id)? != self.roster_digest {
            return Err(Error::Mismatch("roster_digest"));
        }
        Ok(())
    }

    pub fn sign(&self, key: &SigningIdentity, kid: &str) -> Result<SignedPermit> {
        self.check()?;
        Ok(SignedPermit {
            permit: self.clone(),
            kid: kid.to_owned(),
            signature: key.sign(Domain::Permit, &json::to_value(self)?),
        })
    }

    /// The report `reporter` makes of this attempt. Frames are required for
    /// a native win or draw and absent otherwise.
    pub fn report(&self, reporter: &EmberId, observation: Observation) -> Result<GameReport> {
        let opponent = if *reporter == self.p1_id {
            self.p2_id.clone()
        } else if *reporter == self.p2_id {
            self.p1_id.clone()
        } else {
            return Err(Error::Mismatch("reporter is not a fighter"));
        };
        let report = GameReport {
            version: crate::report::VERSION,
            bridge_id: self.bridge_id.clone(),
            match_id: self.match_id.clone(),
            assignment_generation: self.assignment_generation,
            attempt_id: self.attempt_id.clone(),
            permit_id: self.permit_id.clone(),
            observation_id: observation.observation_id,
            room_id: self.room_id.clone(),
            table_id: self.table_id,
            match_generation: self.match_generation,
            reporter_id: reporter.clone(),
            opponent_id: opponent,
            p1_id: self.p1_id.clone(),
            p2_id: self.p2_id.clone(),
            rules_digest: self.rules_digest.clone(),
            roster_digest: self.roster_digest.clone(),
            build_id: self.build_id.clone(),
            result: observation.result,
            capture_frame: observation.capture_frame,
            confirmed_input_frame: observation.confirmed_input_frame,
            observed_at: observation.observed_at,
            helper_instance_id: observation.helper_instance_id,
        };
        report.check()?;
        Ok(report)
    }

    /// True when `report` is about this attempt, field for field.
    pub fn covers(&self, report: &GameReport) -> bool {
        report.bridge_id == self.bridge_id
            && report.match_id == self.match_id
            && report.assignment_generation == self.assignment_generation
            && report.attempt_id == self.attempt_id
            && report.permit_id == self.permit_id
            && report.room_id == self.room_id
            && report.table_id == self.table_id
            && report.match_generation == self.match_generation
            && report.p1_id == self.p1_id
            && report.p2_id == self.p2_id
            && report.rules_digest == self.rules_digest
            && report.roster_digest == self.roster_digest
            && report.build_id == self.build_id
    }
}

/// What one fighter's game saw of an attempt.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Observation {
    pub observation_id: String,
    pub result: Outcome,
    pub capture_frame: Option<Counter>,
    pub confirmed_input_frame: Option<Counter>,
    pub observed_at: u64,
    pub helper_instance_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SignedPermit {
    pub permit: Permit,
    pub kid: String,
    pub signature: String,
}

impl SignedPermit {
    pub fn verify(&self, key: &PublicKey, kid: &str) -> Result<&Permit> {
        if self.kid != kid {
            return Err(Error::Mismatch("kid"));
        }
        self.permit.check()?;
        key.verify(
            Domain::Permit,
            &json::to_value(&self.permit)?,
            &self.signature,
        )?;
        Ok(&self.permit)
    }
}

/// How long a browser handoff stays redeemable (spec 12.1, 25.3).
pub const HANDOFF_SECS: u64 = 60;
/// A handoff code: 32 random bytes as unpadded base64url.
pub const HANDOFF_LEN: usize = 43;

/// True for text shaped like a handoff code. Only the bridge can say whether
/// it names a live handoff.
pub fn is_handoff(text: &str) -> bool {
    decode_b64u::<32>(text, "handoff").is_ok()
}

/// The `ember:` link a browser opens to hand a match to Ember.
pub fn handoff_uri(bridge_id: &str, handoff: &str) -> String {
    format!("ember://tournament/open?bridge={bridge_id}&handoff={handoff}")
}

/// `POST /v1/handoffs`: a provider or organizer asks for a handoff that lets
/// one assigned player open a match in Ember.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateHandoff {
    pub match_id: String,
    pub ember_id: EmberId,
}

impl CreateHandoff {
    pub fn check(&self) -> Result<()> {
        if !is_prefixed_id(&self.match_id, "emt") {
            return Err(Error::InvalidField("match_id"));
        }
        Ok(())
    }
}

/// The handoff, shown once. The bridge keeps only a keyed hash of it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HandoffCreated {
    pub handoff: String,
    pub uri: String,
    pub bridge_id: String,
    pub match_id: String,
    pub expires_at: u64,
}

/// `POST /v1/handoffs/redeem`, signed by the expected player's identity.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RedeemHandoff {
    pub handoff: String,
}

impl RedeemHandoff {
    pub fn check(&self) -> Result<()> {
        if !is_handoff(&self.handoff) {
            return Err(Error::InvalidField("handoff"));
        }
        Ok(())
    }
}

/// What a redeemed handoff names: the match to open. Joining it still goes
/// through the claim and the room's own admission.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HandoffRedeemed {
    pub match_id: String,
}

#[cfg(test)]
mod tests {
    use zeroize::Zeroizing;

    use super::*;

    fn key(byte: u8) -> SigningIdentity {
        SigningIdentity::from_seed(&Zeroizing::new([byte; 32])).unwrap()
    }

    fn rules() -> Rules {
        Rules {
            games_to_win: 2,
            draw_policy: "replay_no_score".into(),
            native_rules_profile: PROFILE.into(),
            edition_policy: "ultra_only".into(),
            character_policy: "unrestricted_between_games".into(),
            stage_policy: "p1_selects".into(),
            input_delay_policy: "ember_existing_ready_policy".into(),
        }
    }

    fn permit(p1: &EmberId, p2: &EmberId) -> Permit {
        Permit {
            version: VERSION,
            bridge_id: "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11".into(),
            match_id: "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12".into(),
            assignment_generation: Counter(1),
            attempt_id: "ega_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a13".into(),
            permit_id: "per_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a14".into(),
            room_id: "0123456789abcdef0123456789abcdef".into(),
            table_id: TABLE,
            match_generation: Counter(7),
            p1_id: p1.clone(),
            p2_id: p2.clone(),
            rules_digest: rules_digest(&rules()).unwrap(),
            roster_digest: roster_digest(p1, p2).unwrap(),
            build_id: "build".into(),
            issued_at: 100,
            start_by: 100 + PERMIT_START_SECS,
        }
    }

    #[test]
    fn permits_verify_only_under_their_domain_and_key() {
        let bridge = key(9);
        let (a, b) = (key(1), key(2));
        let signed = permit(a.ember_id(), b.ember_id())
            .sign(&bridge, "k1")
            .unwrap();
        assert!(signed.verify(&bridge.public_key(), "k1").is_ok());
        assert!(signed.verify(&bridge.public_key(), "k2").is_err());
        assert!(signed.verify(&a.public_key(), "k1").is_err());
        let mut tampered = signed.clone();
        tampered.permit.match_generation = Counter(8);
        assert!(tampered.verify(&bridge.public_key(), "k1").is_err());
        // A permit signature is not a binding signature.
        let as_binding = Domain::Binding.signing_bytes(&json::to_value(&signed.permit).unwrap());
        let as_permit = Domain::Permit.signing_bytes(&json::to_value(&signed.permit).unwrap());
        assert_ne!(as_binding, as_permit);
    }

    #[test]
    fn reports_built_from_a_permit_agree_and_are_covered() {
        let (a, b) = (key(1), key(2));
        let permit = permit(a.ember_id(), b.ember_id());
        let observe = |id: &str| Observation {
            observation_id: id.into(),
            result: Outcome::P1Win,
            capture_frame: Some(Counter(900)),
            confirmed_input_frame: Some(Counter(899)),
            observed_at: 200,
            helper_instance_id: "ins_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a15".into(),
        };
        let mine = permit
            .report(
                a.ember_id(),
                observe("obs_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a16"),
            )
            .unwrap();
        let theirs = permit
            .report(
                b.ember_id(),
                observe("obs_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a17"),
            )
            .unwrap();
        assert!(mine.agrees_with(&theirs) && permit.covers(&mine) && permit.covers(&theirs));
        assert_eq!(mine.opponent_id, *b.ember_id());
        assert!(mine.sign(&a).unwrap().verify().is_ok());
        assert!(
            permit
                .report(
                    key(3).ember_id(),
                    observe("obs_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a18")
                )
                .is_err()
        );
    }

    #[test]
    fn bindings_name_two_distinct_fighters() {
        let (a, b) = (key(1), key(2));
        let mut binding = Binding {
            version: VERSION,
            bridge_id: "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11".into(),
            match_id: "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12".into(),
            assignment_generation: Counter(1),
            binding_revision: Counter(1),
            room_id: "0123456789abcdef0123456789abcdef".into(),
            build_id: "build".into(),
            games_to_win: 2,
            rules_digest: rules_digest(&rules()).unwrap(),
            roster_digest: roster_digest(a.ember_id(), b.ember_id()).unwrap(),
            fighters: [
                BoundFighter {
                    ember_id: a.ember_id().clone(),
                    endpoint_id: "a".repeat(64),
                },
                BoundFighter {
                    ember_id: b.ember_id().clone(),
                    endpoint_id: "b".repeat(64),
                },
            ],
            issued_at: 100,
        };
        assert!(binding.check().is_ok());
        assert_eq!(binding.slot_of(&"b".repeat(64)), Some(1));
        assert_eq!(binding.slot_of(&"c".repeat(64)), None);
        binding.fighters[1].endpoint_id = "a".repeat(64);
        assert!(binding.check().is_err());
        binding.fighters[1].endpoint_id = "b".repeat(64);
        binding.fighters.swap(0, 1);
        assert_eq!(binding.check(), Err(Error::Mismatch("roster_digest")));
    }

    #[test]
    fn publishing_takes_a_lease_a_replaced_room_or_neither() {
        let room = PublishRoom {
            room_id: "0123456789abcdef0123456789abcdef".into(),
            invitation: "sf4e3:abc".into(),
            lease_id: Some("lse_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a19".into()),
            fence: Some(Counter(1)),
            replaces: None,
        };
        assert!(room.check().is_ok());
        let refresh = PublishRoom {
            lease_id: None,
            fence: None,
            ..room.clone()
        };
        assert!(refresh.check().is_ok());
        let half = PublishRoom {
            fence: None,
            ..room.clone()
        };
        assert!(half.check().is_err());
        let both = PublishRoom {
            replaces: Some("f".repeat(32)),
            ..room.clone()
        };
        assert!(both.check().is_err());
        let replacing = PublishRoom {
            replaces: Some("f".repeat(32)),
            ..refresh
        };
        assert!(replacing.check().is_ok());
    }
}
