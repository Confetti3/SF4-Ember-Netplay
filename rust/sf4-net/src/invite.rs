//! Private invitations contain routing data and a room capability, never keys.
use std::{fmt, io};

use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use iroh::{EndpointAddr, EndpointId, RelayUrl, TransportAddr};
use serde::{Deserialize, Serialize};
use subtle::ConstantTimeEq;

use crate::wire::VERSION;

pub const MAX_INVITE_LENGTH: usize = 4096;
// Every invitation is stamped with this lifetime, a week, and members renew
// theirs while the room is open, so a copied link stays good for close to a
// week. A peer of another build never parses these invitations (the build
// check refuses them first), so raising the cap from 1.0.0's 24 hours cannot
// make an older client misread one.
pub const MAX_INVITE_LIFETIME_SECS: u64 = 7 * 24 * 60 * 60;
/// How far behind the issuer's clock a reader's may be. An invitation is
/// stamped with up to a week from the issuer's clock, so a reader whose clock
/// is slower sees more than a week left; up to a day more is still taken as
/// a real invitation. Expiry itself is never relaxed.
pub const MAX_CLOCK_SKEW_SECS: u64 = 24 * 60 * 60;
const PREFIX: &str = "sf4e2:";
const RECOVERY_PREFIX: &str = "sf4e3:";
const LEGACY_PREFIX: &str = "sf4e1:";
const DISCORD_PREFIX: &str = "emd1:";
const RECOVERY_DISCORD_PREFIX: &str = "emd2:";
/// The bytes of a compact record before its build: all a Discord text
/// carries.
const HEADER: usize = 91;
/// The same for an sf4e3 record, which adds the coordination route.
const RECOVERY_HEADER: usize = HEADER + 32 + 8 + 8;
// Stable wire IDs, independent of relay-map iteration order, each with the
// region code (`relay::REGIONS`) the native side and the bridge show for it.
// Append only: never reorder, remove or reuse an entry. A peer of another
// build never reads these invitations (the build check refuses them first),
// so an appended entry needs no new invite version.
const RELAYS: [(&str, &str); 5] = [
    ("https://use1-1.relay.n0.iroh.link./", "use1"),
    ("https://usw1-1.relay.n0.iroh.link./", "usw1"),
    ("https://euc1-1.relay.n0.iroh.link./", "euc1"),
    ("https://aps1-1.relay.n0.iroh.link./", "aps1"),
    // Ember's own relay, on the VPS in Kansas: both US regions reach it.
    ("https://relay.embernetplay.link./", "use1"),
];

/// The RELAYS entries this build's endpoints use, one per region they cover.
/// Some US internet providers' router filters (Xfinity, Spectrum, Cox) block
/// n0's US and Asia relays over HTTPS since 2026-11-08, and iroh picks a home
/// relay by latency alone, so a US player must not be able to land on one:
/// Ember's relay replaces both n0 US relays. Europe and Asia stay on n0's.
const ACTIVE: [usize; 3] = [4, 2, 3];

fn pinned(index: usize) -> RelayUrl {
    RELAYS[index].0.parse().expect("pinned relay URL")
}

/// The relays every endpoint of this build uses, in place of iroh's default
/// map. Each one also answers QUIC address discovery on iroh's default port.
pub fn relay_map() -> iroh::RelayMap {
    ACTIVE
        .iter()
        .map(|&index| iroh::RelayConfig::from(pinned(index)))
        .collect()
}

/// The region code of a pinned relay, or "other" for any other URL. The
/// native side shows these instead of a relay address.
pub fn relay_region(relay: &RelayUrl) -> &'static str {
    RELAYS
        .iter()
        .find(|(url, _)| url.parse::<RelayUrl>().is_ok_and(|pinned| pinned == *relay))
        .map_or("other", |(_, code)| code)
}

/// The relay this build uses for a region code: `relay_region`'s inverse
/// over `relay_map`. None for "other" or any code no active relay covers.
pub fn relay_for_region(code: &str) -> Option<RelayUrl> {
    ACTIVE
        .iter()
        .find(|&&index| RELAYS[index].1 == code)
        .map(|&index| pinned(index))
}

/// The route a selected path reports outside the helper: `direct` for an IP
/// path, `relay:<region>` for a relay path (the code of `relay_region`), and
/// `unavailable` when no path is selected. It never carries an address, a port
/// or a relay URL, so no event, log or export built from it can either.
pub fn public_route(remote: Option<&TransportAddr>) -> String {
    match remote {
        Some(TransportAddr::Ip(_)) => "direct".into(),
        Some(TransportAddr::Relay(relay)) => format!("relay:{}", relay_region(relay)),
        _ => "unavailable".into(),
    }
}

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

/// The payload of a Discord text, emd1 or emd2.
fn discord_payload(text: &str) -> Option<&str> {
    text.strip_prefix(DISCORD_PREFIX)
        .or_else(|| text.strip_prefix(RECOVERY_DISCORD_PREFIX))
}

fn relay_allowed(relay: &RelayUrl) -> bool {
    // Match entire normalized URLs from the pinned dictionary. Suffix checks
    // would admit credentials, arbitrary paths, ports, and look-alike hosts.
    RELAYS
        .iter()
        .any(|(url, _)| url.parse::<RelayUrl>().is_ok_and(|pinned| pinned == *relay))
}

impl Invite {
    /// Discord carries the routing/capability header only. The caller supplies
    /// its actual local build; the host still checks that build in RoomProof.
    pub fn parse_for_build(text: &str, now: u64, build: &str) -> io::Result<Self> {
        // Clipboard sharing can append CRLF. Normalize only the surrounding
        // whitespace; keep the raw input bound and all token validation intact.
        if text.len() > MAX_INVITE_LENGTH {
            return Err(invalid());
        }
        let text = text.trim_matches(|character: char| character.is_ascii_whitespace());
        if let Some(payload) = discord_payload(text) {
            if text.len() != 127 || build.is_empty() || build.len() > 128 {
                return Err(invalid());
            }
            let invite = Self::decode_discord(payload, build)?;
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
    /// The text carries the first `HEADER` bytes of the record alone, for a
    /// recovery invitation too: neither its route nor its build.
    pub fn encode_discord(&self) -> io::Result<String> {
        let bytes = self.compact_bytes()?;
        let prefix = if self.coordination_endpoint.is_some() {
            RECOVERY_DISCORD_PREFIX
        } else {
            DISCORD_PREFIX
        };
        Ok(format!("{prefix}{}", URL_SAFE_NO_PAD.encode(&bytes[..HEADER])))
    }

    pub fn create(
        endpoint: EndpointId,
        relay: RelayUrl,
        build: String,
        now: u64,
        lifetime: u64,
    ) -> io::Result<Self> {
        Self::create_in_room(endpoint, relay, build, now, lifetime, rand::random())
    }

    /// `create` for a room whose id the caller chose, as a public room's is.
    /// The capability is still random. An all-zero room id is refused.
    pub fn create_in_room(
        endpoint: EndpointId,
        relay: RelayUrl,
        build: String,
        now: u64,
        lifetime: u64,
        room: [u8; 16],
    ) -> io::Result<Self> {
        if lifetime == 0 || lifetime > MAX_INVITE_LIFETIME_SECS {
            return Err(invalid());
        }
        let invite = Self {
            version: VERSION,
            endpoint,
            relay,
            room,
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
            || self.expires - now > MAX_INVITE_LIFETIME_SECS + MAX_CLOCK_SKEW_SECS
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

    /// Why `parse_for_build` refused `text`, for the player's message:
    /// `old_version`, `other_build`, `expired` or `malformed`. Only this
    /// word leaves the helper, never the invitation.
    pub fn rejection_reason(text: &str, now: u64, build: &str) -> &'static str {
        let text = text.trim_matches(|character: char| character.is_ascii_whitespace());
        let invite = if text.len() > MAX_INVITE_LENGTH {
            None
        } else if let Some(payload) = discord_payload(text) {
            // A Discord invitation carries no build; it takes the local one.
            Self::decode_discord(payload, build).ok()
        } else {
            Self::decode_text(text).ok()
        };
        match invite {
            Some(invite) if invite.version != VERSION => "old_version",
            // Cut at a whole Base64 group, a compact invitation still decodes,
            // with only the start of this build: the text is short, not foreign.
            Some(invite)
                if invite.build.len() < build.len() && build.starts_with(&invite.build) =>
            {
                "malformed"
            }
            Some(invite) if invite.build != build => "other_build",
            Some(invite) if invite.expires <= now => "expired",
            _ => "malformed",
        }
    }

    pub fn parse(text: &str, now: u64) -> io::Result<Self> {
        if text.len() > MAX_INVITE_LENGTH {
            return Err(invalid());
        }
        let invite = Self::decode_text(text)?;
        invite.validate(now)?;
        Ok(invite)
    }

    /// The invitation an sf4e2, sf4e3 or legacy sf4e1 text holds, before
    /// `validate`. Any other text, a Discord one included, is refused.
    fn decode_text(text: &str) -> io::Result<Self> {
        let decode = |payload: &str| URL_SAFE_NO_PAD.decode(payload).map_err(|_| invalid());
        if let Some(payload) = text.strip_prefix(PREFIX) {
            Self::decode_compact(&decode(payload)?)
        } else if let Some(payload) = text.strip_prefix(RECOVERY_PREFIX) {
            Self::decode_recovery_compact(&decode(payload)?)
        } else {
            let bytes = decode(text.strip_prefix(LEGACY_PREFIX).ok_or_else(invalid)?)?;
            serde_json::from_slice(&bytes).map_err(|_| invalid())
        }
    }

    /// The invitation a Discord text's `payload` holds, before `validate`:
    /// exactly the record's header, which takes the reader's `build`.
    fn decode_discord(payload: &str, build: &str) -> io::Result<Self> {
        let mut bytes = URL_SAFE_NO_PAD.decode(payload).map_err(|_| invalid())?;
        if bytes.len() != HEADER {
            return Err(invalid());
        }
        bytes.extend_from_slice(build.as_bytes());
        Self::decode_compact(&bytes)
    }

    fn decode_compact(bytes: &[u8]) -> io::Result<Self> {
        Self::decode_record(bytes, false)
    }

    fn decode_recovery_compact(bytes: &[u8]) -> io::Result<Self> {
        Self::decode_record(bytes, true)
    }

    /// A compact record: LE protocol version (2), endpoint (32), relay ID
    /// (1), room (16), capability (32), LE expiry (8), the `HEADER`; sf4e3
    /// (`recovery`) extends it with the authenticated coordination endpoint
    /// (32) and the committed LE authority term (8) and incarnation (8); then
    /// the UTF-8 build ID (1..128).
    fn decode_record(bytes: &[u8], recovery: bool) -> io::Result<Self> {
        let build_at = if recovery { RECOVERY_HEADER } else { HEADER };
        if !(build_at + 1..=build_at + 128).contains(&bytes.len()) {
            return Err(invalid());
        }
        let u64_at = |at: usize| -> io::Result<u64> {
            Ok(u64::from_le_bytes(
                bytes[at..at + 8].try_into().map_err(|_| invalid())?,
            ))
        };
        let endpoint_at = |at: usize| -> io::Result<EndpointId> {
            EndpointId::from_bytes(bytes[at..at + 32].try_into().map_err(|_| invalid())?)
                .map_err(|_| invalid())
        };
        let (coordination_endpoint, authority_term, authority_incarnation) = if recovery {
            (Some(endpoint_at(91)?), u64_at(123)?, u64_at(131)?)
        } else {
            (None, 0, 0)
        };
        Ok(Self {
            version: u16::from_le_bytes(bytes[0..2].try_into().map_err(|_| invalid())?),
            endpoint: endpoint_at(2)?,
            relay: RELAYS
                .get(bytes[34] as usize)
                .ok_or_else(invalid)?
                .0
                .parse()
                .map_err(|_| invalid())?,
            room: bytes[35..51].try_into().map_err(|_| invalid())?,
            capability: bytes[51..83].try_into().map_err(|_| invalid())?,
            expires: u64_at(83)?,
            build: std::str::from_utf8(&bytes[build_at..])
                .map_err(|_| invalid())?
                .to_owned(),
            coordination_endpoint,
            authority_term,
            authority_incarnation,
        })
    }

    /// Call only for explicit clipboard/invite export. Never log the result.
    pub fn encode(&self) -> io::Result<String> {
        let bytes = self.compact_bytes()?;
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

    /// The compact record `decode_record` reads.
    fn compact_bytes(&self) -> io::Result<Vec<u8>> {
        let relay = RELAYS
            .iter()
            .position(|(url, _)| {
                url.parse::<RelayUrl>()
                    .is_ok_and(|relay| relay == self.relay)
            })
            .ok_or_else(invalid)?;
        if self.build.is_empty() || self.build.len() > 128 {
            return Err(invalid());
        }
        let mut bytes = Vec::with_capacity(RECOVERY_HEADER + self.build.len());
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
        Ok(bytes)
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

    pub fn expires(&self) -> u64 {
        self.expires
    }

    /// The same room, route and capability, valid for `lifetime` from `now`.
    /// Only a member of the open room renews its own copy; a joiner never
    /// sees anything but the expiry written into the text it was given.
    pub fn renewed(mut self, now: u64, lifetime: u64) -> io::Result<Self> {
        if lifetime == 0 || lifetime > MAX_INVITE_LIFETIME_SECS {
            return Err(invalid());
        }
        self.expires = now.checked_add(lifetime).ok_or_else(invalid)?;
        self.validate(now)?;
        Ok(self)
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

    /// The room's short code (`short_invite`). Every member's copy, renewed
    /// or rerouted, gives the same code, since room and capability never
    /// change while the room is open. Never log the result.
    pub fn short_code(&self) -> String {
        crate::short_invite::code_from_secrets(&self.room, &self.capability)
    }

    pub(crate) fn proof(&self) -> RoomProof {
        self.proof_for(false)
    }

    /// The proof of a joiner that only watches a tournament match when
    /// `observer`, which only a room bound to one takes.
    pub(crate) fn proof_for(&self, observer: bool) -> RoomProof {
        RoomProof {
            version: VERSION,
            room: self.room,
            capability: self.capability,
            build: proof_build(&self.build),
            observer,
        }
    }

    /// Host checks its original room policy; a client cannot extend expiry by
    /// editing the expiry field in a copied ticket.
    pub(crate) fn admit(&self, proof: &RoomProof, now: u64) -> io::Result<()> {
        self.validate(now)?;
        self.admit_member(proof)
    }

    /// Only the service's current endpoint/incarnation membership gate may
    /// use this path. Expiry still applies to every new-member invitation.
    pub(crate) fn admit_member(&self, proof: &RoomProof) -> io::Result<()> {
        self.validate(self.expires.saturating_sub(1))?;
        if proof.version != VERSION
            || proof.room != self.room
            || proof.build != proof_build(&self.build)
            || !bool::from(proof.capability.ct_eq(&self.capability))
        {
            return Err(invalid());
        }
        Ok(())
    }
}

/// The build a room proof names: the invitation's build, the Sidecar.dll
/// hash, followed by this helper's coordination wire version. The two
/// binaries ship together, but the Sidecar hash alone would not tell two
/// packages apart that share a Sidecar.dll and differ in their helper, so
/// the host refuses a joiner whose helper speaks another coordination wire,
/// before any coordination link is made. The invitation text itself still
/// carries the Sidecar hash alone, which keeps its form and length.
fn proof_build(build: &str) -> String {
    format!(
        "{build}+coordination{}",
        crate::coordination::COORDINATION_WIRE_VERSION
    )
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct RoomProof {
    pub version: u16,
    pub room: [u8; 16],
    pub capability: [u8; 32],
    pub build: String,
    /// The joiner watches a tournament match, so only a room bound to one
    /// takes it. A courtesy, not a security property: a bound room admits by
    /// endpoint whatever this says. Left out when false, so every other
    /// proof is the bytes it always was.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub observer: bool,
}

#[cfg(test)]
mod tests;
