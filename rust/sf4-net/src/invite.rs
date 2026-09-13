//! Private invitations contain routing data and a room capability, never keys.
use std::{fmt, io};

use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use iroh::{EndpointAddr, EndpointId, RelayUrl};
use serde::{Deserialize, Serialize};
use subtle::ConstantTimeEq;

use crate::wire::VERSION;

pub const MAX_INVITE_LENGTH: usize = 4096;
pub const MAX_INVITE_LIFETIME_SECS: u64 = 24 * 60 * 60;
const PREFIX: &str = "sf4e2:";
const RECOVERY_PREFIX: &str = "sf4e3:";
const LEGACY_PREFIX: &str = "sf4e1:";
const DISCORD_PREFIX: &str = "emd1:";
const RECOVERY_DISCORD_PREFIX: &str = "emd2:";
// Stable wire IDs, independent of relay-map iteration order. Never reorder or
// reuse these entries; a different dictionary requires a new invite version.
const RELAYS: [&str; 4] = [
    "https://use1-1.relay.n0.iroh.link./",
    "https://usw1-1.relay.n0.iroh.link./",
    "https://euc1-1.relay.n0.iroh.link./",
    "https://aps1-1.relay.n0.iroh.link./",
];

#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Invite {
    version: u16,
    endpoint: EndpointId,
    relay: RelayUrl,
    room: [u8; 16],
    capability: [u8; 32],
    expires: u64,
    build: String,
    #[serde(default)]
    coordination_endpoint: Option<EndpointId>,
    #[serde(default)]
    authority_term: u64,
    #[serde(default)]
    authority_incarnation: u64,
}

// Keep secrets out of errors, tracing, and normal diagnostic exports.
impl fmt::Debug for Invite {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Invite")
            .field("version", &self.version)
            .field("expires", &self.expires)
            .finish_non_exhaustive()
    }
}

fn invalid() -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidInput,
        "invalid, incompatible, or expired invitation",
    )
}

fn relay_allowed(relay: &RelayUrl) -> bool {
    // Match entire normalized URLs from the pinned Iroh release. Suffix checks
    // would admit credentials, arbitrary paths, ports, and look-alike hosts.
    iroh::defaults::prod::default_relay_map().contains(relay)
}

impl Invite {
    /// Discord carries the routing/capability header only. The caller supplies
    /// its actual local build; the host still checks that build in RoomProof.
    pub fn parse_for_build(text: &str, now: u64, build: &str) -> io::Result<Self> {
        if let Some(payload) = text
            .strip_prefix(DISCORD_PREFIX)
            .or_else(|| text.strip_prefix(RECOVERY_DISCORD_PREFIX))
        {
            if text.len() != 127 || build.is_empty() || build.len() > 128 {
                return Err(invalid());
            }
            let mut bytes = URL_SAFE_NO_PAD.decode(payload).map_err(|_| invalid())?;
            if bytes.len() != 91 {
                return Err(invalid());
            }
            bytes.extend_from_slice(build.as_bytes());
            let invite = Self::decode_compact(&bytes)?;
            invite.validate(now)?;
            Ok(invite)
        } else {
            let invite = Self::parse(text, now)?;
            if invite.build != build {
                return Err(invalid());
            }
            Ok(invite)
        }
    }

    /// Export only to the authenticated local Discord bridge, never logs.
    pub fn encode_discord(&self) -> io::Result<String> {
        let encoded = self.encode()?;
        let bytes = URL_SAFE_NO_PAD
            .decode(&encoded[PREFIX.len()..])
            .map_err(|_| invalid())?;
        let prefix = if self.coordination_endpoint.is_some() {
            RECOVERY_DISCORD_PREFIX
        } else {
            DISCORD_PREFIX
        };
        Ok(format!("{prefix}{}", URL_SAFE_NO_PAD.encode(&bytes[..91])))
    }

    pub fn create(
        endpoint: EndpointId,
        relay: RelayUrl,
        build: String,
        now: u64,
        lifetime: u64,
    ) -> io::Result<Self> {
        if lifetime == 0 || lifetime > MAX_INVITE_LIFETIME_SECS {
            return Err(invalid());
        }
        let invite = Self {
            version: VERSION,
            endpoint,
            relay,
            room: rand::random(),
            capability: rand::random(),
            expires: now.checked_add(lifetime).ok_or_else(invalid)?,
            build,
            coordination_endpoint: None,
            authority_term: 0,
            authority_incarnation: 0,
        };
        invite.validate(now)?;
        Ok(invite)
    }

    fn validate(&self, now: u64) -> io::Result<()> {
        if self.version != VERSION
            || self.room == [0; 16]
            || self.capability == [0; 32]
            || self.expires <= now
            || self.expires - now > MAX_INVITE_LIFETIME_SECS
            || self.build.is_empty()
            || self.build.len() > 128
            || !relay_allowed(&self.relay)
            || self.coordination_endpoint.is_some_and(|endpoint| {
                endpoint == self.endpoint
                    || self.authority_term == 0
                    || self.authority_incarnation == 0
            })
        {
            return Err(invalid());
        }
        Ok(())
    }

    pub fn parse(text: &str, now: u64) -> io::Result<Self> {
        if text.len() > MAX_INVITE_LENGTH {
            return Err(invalid());
        }
        let invite = if let Some(payload) = text.strip_prefix(PREFIX) {
            let bytes = URL_SAFE_NO_PAD.decode(payload).map_err(|_| invalid())?;
            Self::decode_compact(&bytes)?
        } else if let Some(payload) = text.strip_prefix(RECOVERY_PREFIX) {
            let bytes = URL_SAFE_NO_PAD.decode(payload).map_err(|_| invalid())?;
            Self::decode_recovery_compact(&bytes)?
        } else {
            let bytes = URL_SAFE_NO_PAD
                .decode(text.strip_prefix(LEGACY_PREFIX).ok_or_else(invalid)?)
                .map_err(|_| invalid())?;
            serde_json::from_slice(&bytes).map_err(|_| invalid())?
        };
        invite.validate(now)?;
        Ok(invite)
    }

    fn decode_compact(bytes: &[u8]) -> io::Result<Self> {
        // LE protocol version (2), endpoint (32), relay ID (1), room (16),
        // capability (32), LE expiry (8), then the UTF-8 build ID (1..128).
        if !(92..=219).contains(&bytes.len()) {
            return Err(invalid());
        }
        Ok(Self {
            version: u16::from_le_bytes(bytes[0..2].try_into().map_err(|_| invalid())?),
            endpoint: EndpointId::from_bytes(bytes[2..34].try_into().map_err(|_| invalid())?)
                .map_err(|_| invalid())?,
            relay: RELAYS
                .get(bytes[34] as usize)
                .ok_or_else(invalid)?
                .parse()
                .map_err(|_| invalid())?,
            room: bytes[35..51].try_into().map_err(|_| invalid())?,
            capability: bytes[51..83].try_into().map_err(|_| invalid())?,
            expires: u64::from_le_bytes(bytes[83..91].try_into().map_err(|_| invalid())?),
            build: std::str::from_utf8(&bytes[91..])
                .map_err(|_| invalid())?
                .to_owned(),
            coordination_endpoint: None,
            authority_term: 0,
            authority_incarnation: 0,
        })
    }

    fn decode_recovery_compact(bytes: &[u8]) -> io::Result<Self> {
        // sf4e3 extends the sf4e2 record with the authenticated coordination
        // endpoint and the committed authority term/incarnation.
        if !(140..=267).contains(&bytes.len()) {
            return Err(invalid());
        }
        Ok(Self {
            version: u16::from_le_bytes(bytes[0..2].try_into().map_err(|_| invalid())?),
            endpoint: EndpointId::from_bytes(bytes[2..34].try_into().map_err(|_| invalid())?)
                .map_err(|_| invalid())?,
            relay: RELAYS
                .get(bytes[34] as usize)
                .ok_or_else(invalid)?
                .parse()
                .map_err(|_| invalid())?,
            room: bytes[35..51].try_into().map_err(|_| invalid())?,
            capability: bytes[51..83].try_into().map_err(|_| invalid())?,
            expires: u64::from_le_bytes(bytes[83..91].try_into().map_err(|_| invalid())?),
            coordination_endpoint: Some(
                EndpointId::from_bytes(bytes[91..123].try_into().map_err(|_| invalid())?)
                    .map_err(|_| invalid())?,
            ),
            authority_term: u64::from_le_bytes(bytes[123..131].try_into().map_err(|_| invalid())?),
            authority_incarnation: u64::from_le_bytes(
                bytes[131..139].try_into().map_err(|_| invalid())?,
            ),
            build: std::str::from_utf8(&bytes[139..])
                .map_err(|_| invalid())?
                .to_owned(),
        })
    }

    /// Call only for explicit clipboard/invite export. Never log the result.
    pub fn encode(&self) -> io::Result<String> {
        let relay = RELAYS
            .iter()
            .position(|url| {
                url.parse::<RelayUrl>()
                    .is_ok_and(|relay| relay == self.relay)
            })
            .ok_or_else(invalid)?;
        if self.build.is_empty() || self.build.len() > 128 {
            return Err(invalid());
        }
        let mut bytes = Vec::with_capacity(139 + self.build.len());
        bytes.extend_from_slice(&self.version.to_le_bytes());
        bytes.extend_from_slice(self.endpoint.as_bytes());
        bytes.push(relay as u8);
        bytes.extend_from_slice(&self.room);
        bytes.extend_from_slice(&self.capability);
        bytes.extend_from_slice(&self.expires.to_le_bytes());
        if let Some(coordination) = self.coordination_endpoint {
            bytes.extend_from_slice(coordination.as_bytes());
            bytes.extend_from_slice(&self.authority_term.to_le_bytes());
            bytes.extend_from_slice(&self.authority_incarnation.to_le_bytes());
        }
        bytes.extend_from_slice(self.build.as_bytes());
        let prefix = if self.coordination_endpoint.is_some() {
            RECOVERY_PREFIX
        } else {
            PREFIX
        };
        let text = format!("{prefix}{}", URL_SAFE_NO_PAD.encode(bytes));
        if text.len() > MAX_INVITE_LENGTH {
            return Err(invalid());
        }
        Ok(text)
    }

    pub fn address(&self) -> EndpointAddr {
        EndpointAddr::new(self.endpoint).with_relay_url(self.relay.clone())
    }

    pub fn coordination_endpoint(&self) -> Option<EndpointId> {
        self.coordination_endpoint
    }

    pub fn coordination_address(&self) -> Option<EndpointAddr> {
        self.coordination_endpoint
            .map(|endpoint| EndpointAddr::new(endpoint).with_relay_url(self.relay.clone()))
    }

    pub fn authority_term(&self) -> u64 {
        self.authority_term
    }

    pub fn authority_incarnation(&self) -> u64 {
        self.authority_incarnation
    }

    pub fn with_authority(
        self,
        coordination_endpoint: EndpointId,
        term: u64,
        incarnation: u64,
    ) -> io::Result<Self> {
        let primary_endpoint = self.endpoint;
        self.with_authority_route(primary_endpoint, coordination_endpoint, term, incarnation)
    }

    /// Refresh both the primary control endpoint and the committed
    /// coordination authority after a leader handoff while preserving the
    /// room capability and expiry.
    pub fn with_authority_route(
        mut self,
        primary_endpoint: EndpointId,
        coordination_endpoint: EndpointId,
        term: u64,
        incarnation: u64,
    ) -> io::Result<Self> {
        if primary_endpoint == coordination_endpoint {
            return Err(invalid());
        }
        self.endpoint = primary_endpoint;
        if coordination_endpoint == self.endpoint || term == 0 || incarnation == 0 {
            return Err(invalid());
        }
        self.coordination_endpoint = Some(coordination_endpoint);
        self.authority_term = term;
        self.authority_incarnation = incarnation;
        self.validate(self.expires.saturating_sub(1))?;
        Ok(self)
    }

    pub fn endpoint(&self) -> EndpointId {
        self.endpoint
    }
    pub fn room(&self) -> [u8; 16] {
        self.room
    }
    pub fn build(&self) -> &str {
        &self.build
    }

    pub(crate) fn proof(&self) -> RoomProof {
        RoomProof {
            version: VERSION,
            room: self.room,
            capability: self.capability,
            build: self.build.clone(),
        }
    }

    /// Host checks its original room policy; a client cannot extend expiry by
    /// editing the expiry field in a copied ticket.
    pub(crate) fn admit(&self, proof: &RoomProof, now: u64) -> io::Result<()> {
        self.validate(now)?;
        if proof.version != VERSION
            || proof.room != self.room
            || proof.build != self.build
            || !bool::from(proof.capability.ct_eq(&self.capability))
        {
            return Err(invalid());
        }
        Ok(())
    }
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct RoomProof {
    pub version: u16,
    pub room: [u8; 16],
    pub capability: [u8; 32],
    pub build: String,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn invite() -> Invite {
        let id = iroh::SecretKey::generate().public();
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        Invite::create(id, relay, "test-sidecar-hash".into(), 100, 3600).unwrap()
    }

    fn legacy(invite: &Invite) -> String {
        format!(
            "{LEGACY_PREFIX}{}",
            URL_SAFE_NO_PAD.encode(serde_json::to_vec(invite).unwrap())
        )
    }

    #[test]
    fn discord_ticket_preserves_capability_and_host_checks_build_and_expiry() {
        for relay in iroh::defaults::prod::default_relay_map().urls::<Vec<_>>() {
            let mut original = invite();
            original.relay = relay;
            original.build = "a".repeat(64);
            let token = original.encode_discord().unwrap();
            assert_eq!(token.len(), 127);
            let guest = Invite::parse_for_build(&token, 101, original.build()).unwrap();
            assert_eq!(guest.room, original.room);
            assert_eq!(guest.endpoint, original.endpoint);
            assert_eq!(guest.relay, original.relay);
            assert_eq!(guest.capability, original.capability);
            assert_eq!(guest.encode_discord().unwrap(), token);
            original.admit(&guest.proof(), 101).unwrap();
            let wrong = Invite::parse_for_build(&token, 101, "wrong-build").unwrap();
            assert!(original.admit(&wrong.proof(), 101).is_err());
            assert!(Invite::parse_for_build(&token, 3700, original.build()).is_err());
            assert!(original.admit(&guest.proof(), 3700).is_err());
            assert!(Invite::parse_for_build(&original.encode().unwrap(), 101, "wrong").is_err());
            assert!(Invite::parse_for_build(&legacy(&original), 101, original.build()).is_ok());
            for length in 0..token.len() {
                assert!(Invite::parse_for_build(&token[..length], 101, original.build()).is_err());
            }
            assert!(
                Invite::parse_for_build(&(token.clone() + "A"), 101, original.build()).is_err()
            );
            assert!(Invite::parse_for_build(&token, 101, "").is_err());
            let mut bytes = URL_SAFE_NO_PAD.decode(&token[5..]).unwrap();
            bytes[34] = 255;
            assert!(
                Invite::parse_for_build(
                    &format!("emd1:{}", URL_SAFE_NO_PAD.encode(bytes)),
                    101,
                    original.build()
                )
                .is_err()
            );
        }
    }

    #[test]
    fn compact_invites_preserve_every_field_and_accept_legacy() {
        for relay in iroh::defaults::prod::default_relay_map().urls::<Vec<_>>() {
            let mut original = invite();
            original.relay = relay;
            original.build = "a".repeat(64); // Production sidecar SHA-256 string.
            let compact = original.encode().unwrap();
            let old = legacy(&original);
            assert_eq!(compact.len(), 213);
            assert!(compact.len() * 2 < old.len());
            for token in [compact, old] {
                let decoded = Invite::parse(&token, 101).unwrap();
                assert_eq!(decoded.endpoint, original.endpoint);
                assert_eq!(decoded.relay, original.relay);
                assert_eq!(decoded.expires, original.expires);
                original.admit(&decoded.proof(), 101).unwrap();
            }
        }
    }

    #[test]
    fn compact_invites_reject_truncation_unknown_relay_and_bad_fields() {
        let original = invite();
        let token = original.encode().unwrap();
        let bytes = URL_SAFE_NO_PAD
            .decode(token.strip_prefix(PREFIX).unwrap())
            .unwrap();
        let encoded = |bytes: &[u8]| format!("{PREFIX}{}", URL_SAFE_NO_PAD.encode(bytes));
        for length in 0..92 {
            assert!(Invite::parse(&encoded(&bytes[..length]), 101).is_err());
        }
        for (offset, value) in [(0, 255), (34, 255), (91, 255)] {
            let mut bad = bytes.clone();
            bad[offset] = value;
            assert!(Invite::parse(&encoded(&bad), 101).is_err());
        }
        for range in [35..51, 51..83] {
            let mut bad = bytes.clone();
            bad[range].fill(0);
            assert!(Invite::parse(&encoded(&bad), 101).is_err());
        }
        assert!(Invite::parse(&encoded(&vec![0; 220]), 101).is_err());
        assert!(Invite::parse(&token, 3700).is_err());
        assert!(Invite::parse(&token, 0).is_ok());
        let mut maximum = original.clone();
        maximum.build = "x".repeat(128);
        assert_eq!(
            Invite::parse(&maximum.encode().unwrap(), 101)
                .unwrap()
                .build(),
            maximum.build()
        );
    }

    #[test]
    fn invite_roundtrips_without_private_addresses_or_secret_key() {
        let original = invite();
        let token = original.encode().unwrap();
        let decoded = Invite::parse(&token, 101).unwrap();
        assert_eq!(decoded.endpoint(), original.endpoint());
        assert!(decoded.address().ip_addrs().next().is_none());
        assert!(original.admit(&decoded.proof(), 101).is_ok());
        assert!(!format!("{decoded:?}").contains(&token));
    }

    #[test]
    fn host_rejects_modified_capability_build_room_and_expired_policy() {
        let original = invite();
        let mut proof = original.proof();
        proof.capability[0] ^= 1;
        assert!(original.admit(&proof, 101).is_err());
        proof = original.proof();
        proof.build.push('x');
        assert!(original.admit(&proof, 101).is_err());
        proof = original.proof();
        proof.room[0] ^= 1;
        assert!(original.admit(&proof, 101).is_err());
        assert!(original.admit(&original.proof(), 3700).is_err());
        let mut extended = original.clone();
        extended.expires = 4000;
        assert!(original.admit(&extended.proof(), 3700).is_err());
    }

    #[test]
    fn untrusted_invites_reject_arbitrary_routing_and_lengths() {
        for relay in [
            "http://localhost/",
            "https://example.com/",
            "https://use1-1.relay.n0.iroh.link.evil/",
            "https://user@use1-1.relay.n0.iroh.link./",
            "https://use1-1.relay.n0.iroh.link./extra",
        ] {
            let mut unsafe_invite = invite();
            unsafe_invite.relay = relay.parse().unwrap();
            assert!(unsafe_invite.encode().is_err());
            assert!(Invite::parse(&legacy(&unsafe_invite), 101).is_err());
        }
        assert!(Invite::parse(&"x".repeat(MAX_INVITE_LENGTH + 1), 101).is_err());
        assert!(Invite::parse("sf4e1:invalid!", 101).is_err());
        let mut wrong = invite();
        wrong.version = 2;
        assert!(Invite::parse(&wrong.encode().unwrap(), 101).is_err());
    }
}
