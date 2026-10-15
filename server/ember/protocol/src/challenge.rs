//! Bridge-issued challenges and the identity proofs that answer them
//! (spec 10.1). A proof binds one stored challenge to one exact command.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::{
    EmberId, Error, PublicKey, Result, SigningIdentity,
    encoding::{OriginPolicy, b64u, check_origin, decode_b64u, is_prefixed_id},
    json::{self, MAX_SAFE_INTEGER, Value},
    sign::Domain,
};

pub const VERSION: u8 = 1;
pub const LIFETIME_SECS: u64 = 60;
/// How far the signer's clock may be from the bridge's (a day, as for
/// invitations); the bridge still holds the challenge to its own window.
pub const SIGN_SKEW_SECS: u64 = 24 * 60 * 60;
/// Link and proof request bodies (spec 25.1).
pub const MAX_PROOF_BODY: usize = 8 * 1024;
const MAX_PATH: usize = 512;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum Action {
    #[serde(rename = "session.create")]
    SessionCreate,
    #[serde(rename = "link.claim")]
    LinkClaim,
    #[serde(rename = "link.cancel")]
    LinkCancel,
    #[serde(rename = "link.remove")]
    LinkRemove,
    #[serde(rename = "match.claim")]
    MatchClaim,
    #[serde(rename = "room.publish")]
    RoomPublish,
    #[serde(rename = "lease.renew")]
    LeaseRenew,
    #[serde(rename = "attempt.prepare")]
    AttemptPrepare,
    #[serde(rename = "discord.connect")]
    DiscordConnect,
    #[serde(rename = "discord.remove")]
    DiscordRemove,
    #[serde(rename = "discord.cancel")]
    DiscordCancel,
}

impl Action {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::SessionCreate => "session.create",
            Self::LinkClaim => "link.claim",
            Self::LinkCancel => "link.cancel",
            Self::LinkRemove => "link.remove",
            Self::MatchClaim => "match.claim",
            Self::RoomPublish => "room.publish",
            Self::LeaseRenew => "lease.renew",
            Self::AttemptPrepare => "attempt.prepare",
            Self::DiscordConnect => "discord.connect",
            Self::DiscordRemove => "discord.remove",
            Self::DiscordCancel => "discord.cancel",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum Method {
    #[serde(rename = "POST")]
    Post,
    #[serde(rename = "DELETE")]
    Delete,
}

impl Method {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Post => "POST",
            Self::Delete => "DELETE",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Challenge {
    pub version: u8,
    pub bridge_id: String,
    pub audience: String,
    pub challenge_id: String,
    pub ember_id: EmberId,
    pub action: Action,
    pub method: Method,
    pub path: String,
    pub request_digest: String,
    pub nonce: String,
    pub issued_at: u64,
    pub expires_at: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Proof {
    pub challenge_id: String,
    pub signature: String,
}

/// What the verifier, or the helper before it signs, expects the challenge
/// to authorize.
#[derive(Clone, Copy, Debug)]
pub struct Expected<'a> {
    pub bridge_id: &'a str,
    pub audience: &'a str,
    pub ember_id: &'a EmberId,
    pub action: Action,
    pub method: Method,
    pub path: &'a str,
    pub command: &'a Value,
}

/// `base64url(SHA-256(JCS(command)))`.
pub fn command_digest(command: &Value) -> String {
    b64u(&Sha256::digest(command.canonical()))
}

/// True for a `/v1/` path made of the characters the schema allows.
pub fn is_api_path(path: &str) -> bool {
    path.len() <= MAX_PATH
        && path.starts_with("/v1/")
        && path.len() > 4
        && path
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'/' | b'-'))
}

impl Challenge {
    /// Field formats and the bounded lifetime, independent of any request.
    pub fn check_shape(&self, policy: OriginPolicy) -> Result<()> {
        if self.version != VERSION {
            return Err(Error::InvalidField("version"));
        }
        if !is_prefixed_id(&self.bridge_id, "brg") {
            return Err(Error::InvalidField("bridge_id"));
        }
        check_origin(&self.audience, policy)?;
        if !is_prefixed_id(&self.challenge_id, "chl") {
            return Err(Error::InvalidField("challenge_id"));
        }
        if !is_api_path(&self.path) {
            return Err(Error::InvalidField("path"));
        }
        decode_b64u::<32>(&self.request_digest, "request_digest")?;
        decode_b64u::<32>(&self.nonce, "nonce")?;
        let lifetime = self.expires_at.saturating_sub(self.issued_at);
        if lifetime == 0 || lifetime > LIFETIME_SECS || self.expires_at > MAX_SAFE_INTEGER as u64 {
            return Err(Error::InvalidField("expires_at"));
        }
        Ok(())
    }

    /// Shape, binding to `expected`, and time window.
    pub fn check(&self, expected: &Expected<'_>, now: u64, policy: OriginPolicy) -> Result<()> {
        self.check_bound(expected, policy)?;
        if now < self.issued_at || now >= self.expires_at {
            return Err(Error::Expired);
        }
        Ok(())
    }

    /// Shape and binding to `expected`, without the time window.
    fn check_bound(&self, expected: &Expected<'_>, policy: OriginPolicy) -> Result<()> {
        self.check_shape(policy)?;
        let binding = [
            (self.bridge_id == expected.bridge_id, "bridge_id"),
            (self.audience == expected.audience, "audience"),
            (&self.ember_id == expected.ember_id, "ember_id"),
            (self.action == expected.action, "action"),
            (self.method == expected.method, "method"),
            (self.path == expected.path, "path"),
            (
                self.request_digest == command_digest(expected.command),
                "request_digest",
            ),
        ];
        if let Some((_, field)) = binding.iter().find(|(matches, _)| !matches) {
            return Err(Error::Mismatch(field));
        }
        Ok(())
    }

    pub fn to_value(&self) -> Result<Value> {
        json::to_value(self)
    }

    /// Signs after checking the challenge against the operation the user
    /// actually started. This is the only way to produce a challenge proof.
    ///
    /// The bridge holds the challenge to its own clock when the proof comes
    /// back, so the signer's clock only has to rule out a challenge that is
    /// plainly stale: within `SIGN_SKEW_SECS` either way it signs, and a player
    /// whose clock is a few seconds or minutes off can still sign in.
    pub fn sign(
        &self,
        identity: &SigningIdentity,
        expected: &Expected<'_>,
        now: u64,
        policy: OriginPolicy,
    ) -> Result<Proof> {
        if expected.ember_id != identity.ember_id() {
            return Err(Error::Mismatch("ember_id"));
        }
        self.check_bound(expected, policy)?;
        if now.saturating_add(SIGN_SKEW_SECS) < self.issued_at
            || now >= self.expires_at.saturating_add(SIGN_SKEW_SECS)
        {
            return Err(Error::Expired);
        }
        Ok(Proof {
            challenge_id: self.challenge_id.clone(),
            signature: identity.sign(Domain::Challenge, &self.to_value()?),
        })
    }

    /// Full verification of a stored challenge against the submitted command
    /// and proof. Single use is the caller's job: consume the stored
    /// challenge in the same transaction as the mutation it authorizes.
    pub fn verify(
        &self,
        expected: &Expected<'_>,
        proof: &Proof,
        key: &PublicKey,
        now: u64,
        policy: OriginPolicy,
    ) -> Result<()> {
        self.check(expected, now, policy)?;
        if key.ember_id() != self.ember_id {
            return Err(Error::Mismatch("ember_id"));
        }
        if proof.challenge_id != self.challenge_id {
            return Err(Error::Mismatch("challenge_id"));
        }
        key.verify(Domain::Challenge, &self.to_value()?, &proof.signature)
    }
}

/// The body of every proof-carrying request: `{"command": ..., "proof": ...}`.
/// `command` stays a strict value so its digest is computed over exactly what
/// was received.
#[derive(Clone, Debug)]
pub struct ProvenRequest {
    pub command: Value,
    pub proof: Proof,
}

impl ProvenRequest {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        let Value::Object(mut body) = json::parse(bytes, MAX_PROOF_BODY)? else {
            return Err(Error::InvalidField("body"));
        };
        let command = body
            .remove("command")
            .ok_or(Error::InvalidField("command"))?;
        let proof = body.remove("proof").ok_or(Error::InvalidField("proof"))?;
        if !body.is_empty() || !matches!(command, Value::Object(_)) {
            return Err(Error::InvalidField("body"));
        }
        Ok(Self {
            command,
            proof: json::from_value(&proof)?,
        })
    }

    pub fn to_json(&self) -> Result<Vec<u8>> {
        let mut body = std::collections::BTreeMap::new();
        body.insert("command".to_owned(), self.command.clone());
        body.insert("proof".to_owned(), json::to_value(&self.proof)?);
        Ok(Value::Object(body).canonical())
    }
}
