//! A fighter's signed statement about one native game (spec 16.2). Nothing
//! in this branch produces reports; the types exist so the bridge, helper and
//! SDK agree on the frozen wire form before WP5 builds on it.
use serde::{Deserialize, Serialize};

use crate::{
    EmberId, Error, PublicKey, Result, SigningIdentity,
    encoding::{Counter, decode_b64u, is_prefixed_id},
    json::{self, Value},
    sign::Domain,
};

pub const VERSION: u8 = 1;
/// Signed native report bodies (spec 25.1).
pub const MAX_REPORT_BODY: usize = 8 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Outcome {
    P1Win,
    P2Win,
    Draw,
    Abort,
    Cancel,
}

impl Outcome {
    /// A native win or draw, which must carry confirmation frames.
    pub fn is_native_result(self) -> bool {
        matches!(self, Self::P1Win | Self::P2Win | Self::Draw)
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct GameReport {
    pub version: u8,
    pub bridge_id: String,
    pub match_id: String,
    pub assignment_generation: Counter,
    pub attempt_id: String,
    pub permit_id: String,
    pub observation_id: String,
    pub room_id: String,
    pub table_id: u8,
    pub match_generation: Counter,
    pub reporter_id: EmberId,
    pub opponent_id: EmberId,
    pub p1_id: EmberId,
    pub p2_id: EmberId,
    pub rules_digest: String,
    pub roster_digest: String,
    pub build_id: String,
    pub result: Outcome,
    pub capture_frame: Option<Counter>,
    pub confirmed_input_frame: Option<Counter>,
    pub observed_at: u64,
    pub helper_instance_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SignedReport {
    pub report: GameReport,
    pub public_key: PublicKey,
    pub signature: String,
}

impl GameReport {
    /// Field formats and internal consistency. A consistent report is still
    /// only what one client says it saw.
    pub fn check(&self) -> Result<()> {
        let ids = [
            (is_prefixed_id(&self.bridge_id, "brg"), "bridge_id"),
            (is_prefixed_id(&self.match_id, "emt"), "match_id"),
            (is_prefixed_id(&self.attempt_id, "ega"), "attempt_id"),
            (is_prefixed_id(&self.permit_id, "per"), "permit_id"),
            (
                is_prefixed_id(&self.observation_id, "obs"),
                "observation_id",
            ),
            (
                is_prefixed_id(&self.helper_instance_id, "ins"),
                "helper_instance_id",
            ),
            (
                self.room_id.len() == 32
                    && self
                        .room_id
                        .bytes()
                        .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)),
                "room_id",
            ),
            (self.version == VERSION, "version"),
            (self.table_id <= 3, "table_id"),
            (
                self.assignment_generation.is_positive(),
                "assignment_generation",
            ),
            (self.match_generation.is_positive(), "match_generation"),
            (
                !self.build_id.is_empty() && self.build_id.len() <= 128,
                "build_id",
            ),
        ];
        if let Some((_, field)) = ids.iter().find(|(valid, _)| !valid) {
            return Err(Error::InvalidField(field));
        }
        decode_b64u::<32>(&self.rules_digest, "rules_digest")?;
        decode_b64u::<32>(&self.roster_digest, "roster_digest")?;
        if self.p1_id == self.p2_id {
            return Err(Error::Mismatch("duplicate fighter"));
        }
        let reporter_is_fighter = self.reporter_id != self.opponent_id
            && [&self.reporter_id, &self.opponent_id]
                .iter()
                .all(|id| **id == self.p1_id || **id == self.p2_id);
        if !reporter_is_fighter {
            return Err(Error::Mismatch("reporter is not a fighter"));
        }
        if self.result.is_native_result() {
            let (Some(capture), Some(confirmed)) = (self.capture_frame, self.confirmed_input_frame)
            else {
                return Err(Error::InvalidField("capture_frame"));
            };
            // Publication boundary: inputs confirmed through at least N - 1.
            if !capture.is_positive() || confirmed.0 < capture.0 - 1 {
                return Err(Error::Mismatch("result inputs are not confirmed"));
            }
        } else if self.capture_frame.is_some_and(|frame| !frame.is_positive()) {
            return Err(Error::InvalidField("capture_frame"));
        }
        Ok(())
    }

    pub fn sign(&self, identity: &SigningIdentity) -> Result<SignedReport> {
        if &self.reporter_id != identity.ember_id() {
            return Err(Error::Mismatch("reporter_id"));
        }
        self.check()?;
        Ok(SignedReport {
            report: self.clone(),
            public_key: identity.public_key(),
            signature: identity.sign(Domain::GameReport, &json::to_value(self)?),
        })
    }

    /// True when two reports describe the same attempt and outcome. Different
    /// observation IDs, times and confirmation frames are not disagreement.
    pub fn agrees_with(&self, other: &GameReport) -> bool {
        self.bridge_id == other.bridge_id
            && self.match_id == other.match_id
            && self.assignment_generation == other.assignment_generation
            && self.attempt_id == other.attempt_id
            && self.permit_id == other.permit_id
            && self.room_id == other.room_id
            && self.table_id == other.table_id
            && self.match_generation == other.match_generation
            && self.p1_id == other.p1_id
            && self.p2_id == other.p2_id
            && self.rules_digest == other.rules_digest
            && self.roster_digest == other.roster_digest
            && self.build_id == other.build_id
            && self.result == other.result
    }
}

impl SignedReport {
    /// Verifies the reporter's key, consistency and signature. The reporter
    /// ID is recomputed from the key, never taken from the report alone.
    pub fn verify(&self) -> Result<&GameReport> {
        if self.public_key.ember_id() != self.report.reporter_id {
            return Err(Error::Mismatch("reporter_id"));
        }
        self.report.check()?;
        let value: Value = json::to_value(&self.report)?;
        self.public_key
            .verify(Domain::GameReport, &value, &self.signature)?;
        Ok(&self.report)
    }
}
