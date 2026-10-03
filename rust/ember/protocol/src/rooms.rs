//! Public rooms: the signed ticket a bridge issues for each join of a listed
//! room, and the objects of the bridge's room routes (see
//! `docs/design/PUBLIC_ROOMS.md`).
//!
//! A ticket is signed with the bridge's own key under its own domain, using
//! the same canonical JSON and Ed25519 rules as bindings and permits. Only a
//! room host verifies it, against the key the bridge gave the supervisor when
//! the room was created.
use serde::{Deserialize, Serialize};

use crate::{
    EmberId, Error, PublicKey, Result, SigningIdentity,
    encoding::is_prefixed_id,
    json,
    play::{MAX_INVITATION, VERSION, check_build, is_hex},
    sign::Domain,
};

/// Refusal reasons of the room routes.
pub const ROOM_LIMIT: &str = "room_limit";
pub const UNSUPPORTED_BUILD: &str = "unsupported_build";
pub const INVALID_NAME: &str = "invalid_name";
pub const ROOM_NOT_FOUND: &str = "room_not_found";
/// A ticket asked for before the room's creator is inside. Like a room that
/// is not found, since until then the room is not public.
pub const ROOM_NOT_OPEN: &str = "room_not_open";
pub const ROOM_FULL: &str = "room_full";
pub const BANNED: &str = "banned";

/// How long a ticket is good for.
pub const TICKET_SECS: u64 = 60;
/// How far ahead of the verifier's clock a ticket may have been issued.
pub const TICKET_SKEW_SECS: u64 = 30;
pub const MAX_NAME: usize = 64;
pub const MIN_CAPACITY: u8 = 2;
pub const MAX_CAPACITY: u8 = 16;
/// Accounts a server-owned room may ban in its lifetime. Bans are never
/// evicted: the room authority closes the room instead of exceeding this.
/// The same number is in `src/roomhost` (the C++ room model),
/// `rust/ember-rooms/src/protocol.rs` (`MAX_ROOM_BANS`, a separate crate) and
/// the helper (`sf4-net` `public_room`, which uses this constant); keep
/// them equal.
pub const MAX_ROOM_BANS: usize = 512;
const MAX_REGION: usize = 8;

/// A room name: 1 to 64 bytes of single-line text with no control characters
/// and no leading or trailing whitespace.
pub fn check_room_name(name: &str) -> Result<()> {
    let valid = !name.is_empty()
        && name.len() <= MAX_NAME
        && !name.chars().any(char::is_control)
        && name.trim() == name;
    if valid {
        Ok(())
    } else {
        Err(Error::InvalidField("name"))
    }
}

fn check_capacity(capacity: u8) -> Result<()> {
    if (MIN_CAPACITY..=MAX_CAPACITY).contains(&capacity) {
        Ok(())
    } else {
        Err(Error::InvalidField("capacity"))
    }
}

/// Admission to one room for one helper endpoint, signed by the bridge.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RoomTicket {
    pub version: u8,
    pub bridge_id: String,
    pub room_id: String,
    pub ember_id: EmberId,
    /// The joiner's helper endpoint.
    pub endpoint_id: String,
    pub issued_at: u64,
    pub expires_at: u64,
}

impl RoomTicket {
    pub fn check(&self) -> Result<()> {
        let fields = [
            (self.version == VERSION, "version"),
            (is_prefixed_id(&self.bridge_id, "brg"), "bridge_id"),
            (is_hex(&self.room_id, 32), "room_id"),
            (is_hex(&self.endpoint_id, 64), "endpoint_id"),
            (
                self.issued_at.checked_add(TICKET_SECS) == Some(self.expires_at),
                "expires_at",
            ),
        ];
        match fields.iter().find(|(valid, _)| !valid) {
            Some((_, field)) => Err(Error::InvalidField(field)),
            None => Ok(()),
        }
    }

    pub fn sign(&self, key: &SigningIdentity, kid: &str) -> Result<SignedRoomTicket> {
        self.check()?;
        Ok(SignedRoomTicket {
            ticket: self.clone(),
            kid: kid.to_owned(),
            signature: key.sign(Domain::RoomTicket, &json::to_value(self)?),
        })
    }

    /// Whether this ticket admits `endpoint_id` to `room_id` at `now` (unix
    /// seconds): the room and endpoint match, it was not issued more than the
    /// allowed skew ahead of `now`, and it has not expired.
    pub fn admits(&self, room_id: &str, endpoint_id: &str, now: u64) -> bool {
        self.room_id == room_id
            && self.endpoint_id == endpoint_id
            && self.issued_at <= now.saturating_add(TICKET_SKEW_SECS)
            && now < self.expires_at
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SignedRoomTicket {
    pub ticket: RoomTicket,
    pub kid: String,
    pub signature: String,
}

impl SignedRoomTicket {
    /// Checks the signature against the bridge key the room was created with
    /// and the ticket's own consistency. It does not look at the clock; see
    /// [`RoomTicket::admits`]. Whether it is the expected bridge is the
    /// caller's check.
    pub fn verify(&self, key: &PublicKey, kid: &str) -> Result<&RoomTicket> {
        if self.kid != kid {
            return Err(Error::Mismatch("kid"));
        }
        self.ticket.check()?;
        key.verify(
            Domain::RoomTicket,
            &json::to_value(&self.ticket)?,
            &self.signature,
        )?;
        Ok(&self.ticket)
    }
}

/// One open room in a listing.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RoomSummary {
    pub room_id: String,
    pub name: String,
    pub build_id: String,
    pub members: u8,
    pub capacity: u8,
    pub tables_playing: u8,
    /// The host's relay region code, such as `use1`.
    pub region: String,
    pub created_at: u64,
}

impl RoomSummary {
    pub fn check(&self) -> Result<()> {
        if !is_hex(&self.room_id, 32) {
            return Err(Error::InvalidField("room_id"));
        }
        check_room_name(&self.name)?;
        check_build(&self.build_id)?;
        check_capacity(self.capacity)?;
        if self.members > self.capacity {
            return Err(Error::InvalidField("members"));
        }
        let region_valid = !self.region.is_empty()
            && self.region.len() <= MAX_REGION
            && self
                .region
                .bytes()
                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit());
        if !region_valid {
            return Err(Error::InvalidField("region"));
        }
        Ok(())
    }
}

/// `GET /v1/rooms`: at most 100 open rooms, fullest last.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RoomList {
    pub rooms: Vec<RoomSummary>,
}

/// `POST /v1/rooms`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateRoom {
    pub name: String,
    pub capacity: u8,
    pub build_id: String,
}

impl CreateRoom {
    pub fn check(&self) -> Result<()> {
        check_room_name(&self.name)?;
        check_capacity(self.capacity)?;
        check_build(&self.build_id)
    }
}

/// `POST /v1/rooms/{room_id}/tickets`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TicketRequest {
    pub endpoint_id: String,
    pub build_id: String,
}

impl TicketRequest {
    pub fn check(&self) -> Result<()> {
        if !is_hex(&self.endpoint_id, 64) {
            return Err(Error::InvalidField("endpoint_id"));
        }
        check_build(&self.build_id)
    }
}

/// What creating or joining a room returns.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RoomAdmission {
    pub room: RoomSummary,
    /// The room host's current `sf4e3` invitation.
    pub invitation: String,
    pub ticket: SignedRoomTicket,
}

impl RoomAdmission {
    /// The pieces' own rules, and that the ticket is for the room listed.
    pub fn check(&self) -> Result<()> {
        self.room.check()?;
        self.ticket.ticket.check()?;
        if self.invitation.is_empty()
            || self.invitation.len() > MAX_INVITATION
            || !self.invitation.is_ascii()
        {
            return Err(Error::InvalidField("invitation"));
        }
        if self.ticket.ticket.room_id != self.room.room_id {
            return Err(Error::Mismatch("room_id"));
        }
        Ok(())
    }
}

/// A creator who is not linked on the calling connection.
pub const NOT_LINKED: &str = "not_linked";
/// Why a room a connection created closed: the connection closed it, or it
/// ended on its own (it emptied, its creator never came, or its host stopped).
pub const CLOSED_BY_CONNECTION: &str = "closed_by_connection";
pub const ENDED: &str = "ended";
const MAX_REASON: usize = 256;

/// The page a room link opens. It hands Ember the room, or offers to install
/// Ember first. The room rides in the fragment, which browsers never send.
pub const ROOM_PAGE: &str = "https://embernetplay.link/r#";

/// A room's link. Not a secret: the room is public, and admission is still
/// the ticket's.
pub fn join_url(bridge_id: &str, room_id: &str) -> String {
    format!("{ROOM_PAGE}{bridge_id}/{room_id}")
}

/// The player a connection opens a room for: linked on that connection.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RoomCreator {
    pub participant_id: String,
    pub ember_id: EmberId,
}

/// `POST /v1/rooms` with a provider credential: a room for a linked player,
/// who goes in first and moderates it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ConnectionCreateRoom {
    pub name: String,
    pub capacity: u8,
    pub build_id: String,
    pub creator: RoomCreator,
}

impl ConnectionCreateRoom {
    /// The room part, as a player's request would carry it.
    pub fn room(&self) -> CreateRoom {
        CreateRoom {
            name: self.name.clone(),
            capacity: self.capacity,
            build_id: self.build_id.clone(),
        }
    }

    pub fn check(&self) -> Result<()> {
        if self.creator.participant_id.is_empty() || self.creator.participant_id.len() > 128 {
            return Err(Error::InvalidField("creator"));
        }
        self.room().check()
    }
}

/// Where a connection's room stands: waiting for its creator, open to anyone,
/// or closed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RoomState {
    Waiting,
    Open,
    Closed,
}

/// A room as the connection that created it sees it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ConnectionRoom {
    pub room: RoomSummary,
    pub state: RoomState,
    pub creator_ember_id: EmberId,
    pub join_url: String,
}

/// `GET /v1/rooms` with a provider credential: the connection's rooms that
/// are not closed, newest first.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ConnectionRoomList {
    pub rooms: Vec<ConnectionRoom>,
}

/// `POST /v1/rooms/{room_id}/close`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CloseRoom {
    pub reason: String,
}

impl CloseRoom {
    pub fn check(&self) -> Result<()> {
        if self.reason.is_empty()
            || self.reason.len() > MAX_REASON
            || self.reason.chars().any(char::is_control)
        {
            return Err(Error::InvalidField("reason"));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use zeroize::Zeroizing;

    use super::*;

    const BRIDGE: &str = "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11";
    const ROOM: &str = "0123456789abcdef0123456789abcdef";

    fn key(byte: u8) -> SigningIdentity {
        SigningIdentity::from_seed(&Zeroizing::new([byte; 32])).unwrap()
    }

    fn ticket(ember_id: &EmberId) -> RoomTicket {
        RoomTicket {
            version: VERSION,
            bridge_id: BRIDGE.into(),
            room_id: ROOM.into(),
            ember_id: ember_id.clone(),
            endpoint_id: "a".repeat(64),
            issued_at: 1000,
            expires_at: 1000 + TICKET_SECS,
        }
    }

    fn summary() -> RoomSummary {
        RoomSummary {
            room_id: ROOM.into(),
            name: "Friendly matches".into(),
            build_id: "build".into(),
            members: 3,
            capacity: 8,
            tables_playing: 1,
            region: "use1".into(),
            created_at: 900,
        }
    }

    #[test]
    fn tickets_verify_only_under_their_domain_key_and_kid() {
        let (bridge, player) = (key(9), key(1));
        let signed = ticket(player.ember_id()).sign(&bridge, "k1").unwrap();
        assert_eq!(
            signed.verify(&bridge.public_key(), "k1").unwrap(),
            &signed.ticket
        );
        assert_eq!(
            signed.verify(&bridge.public_key(), "k2"),
            Err(Error::Mismatch("kid"))
        );
        assert!(signed.verify(&player.public_key(), "k1").is_err());
        let mut tampered = signed.clone();
        tampered.ticket.endpoint_id = "b".repeat(64);
        assert_eq!(
            tampered.verify(&bridge.public_key(), "k1"),
            Err(Error::InvalidSignature)
        );
        let mut tampered = signed.clone();
        tampered.ticket.room_id = "f".repeat(32);
        assert!(tampered.verify(&bridge.public_key(), "k1").is_err());
        let mut tampered = signed;
        tampered.ticket.ember_id = key(2).ember_id().clone();
        assert!(tampered.verify(&bridge.public_key(), "k1").is_err());
    }

    #[test]
    fn a_ticket_signature_is_not_a_binding_or_permit_signature() {
        let (bridge, player) = (key(9), key(1));
        let value = json::to_value(&ticket(player.ember_id())).unwrap();
        let signature = bridge.sign(Domain::RoomTicket, &value);
        let public = bridge.public_key();
        assert!(
            public
                .verify(Domain::RoomTicket, &value, &signature)
                .is_ok()
        );
        for other in [Domain::Binding, Domain::Permit] {
            assert!(public.verify(other, &value, &signature).is_err());
            assert_ne!(
                other.signing_bytes(&value),
                Domain::RoomTicket.signing_bytes(&value)
            );
        }
        assert!(
            Domain::RoomTicket
                .signing_bytes(&value)
                .starts_with(b"EMBER:ROOM-TICKET:1\n")
        );
    }

    #[test]
    fn signing_refuses_an_inconsistent_ticket() {
        let bridge = key(9);
        let id = key(1).ember_id().clone();
        let good = ticket(&id);
        let broken = [
            RoomTicket {
                version: 2,
                ..good.clone()
            },
            RoomTicket {
                bridge_id: "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11".into(),
                ..good.clone()
            },
            RoomTicket {
                room_id: "abc".into(),
                ..good.clone()
            },
            RoomTicket {
                endpoint_id: "A".repeat(64),
                ..good.clone()
            },
            RoomTicket {
                expires_at: good.issued_at + TICKET_SECS + 1,
                ..good.clone()
            },
            RoomTicket {
                expires_at: good.issued_at,
                ..good.clone()
            },
        ];
        for ticket in broken {
            assert!(ticket.sign(&bridge, "k1").is_err(), "{ticket:?}");
        }
        assert!(good.sign(&bridge, "k1").is_ok());
    }

    #[test]
    fn admission_checks_room_endpoint_and_the_clock() {
        let t = ticket(key(1).ember_id());
        let endpoint = "a".repeat(64);
        assert!(t.admits(ROOM, &endpoint, 1000));
        assert!(t.admits(ROOM, &endpoint, 1059));
        assert!(!t.admits(ROOM, &endpoint, 1060));
        assert!(!t.admits(ROOM, &endpoint, 5000));
        assert!(!t.admits(&"f".repeat(32), &endpoint, 1000));
        assert!(!t.admits(ROOM, &"b".repeat(64), 1000));
        // Issued up to 30 s ahead of the verifier's clock is accepted.
        assert!(t.admits(ROOM, &endpoint, 970));
        assert!(!t.admits(ROOM, &endpoint, 969));
        assert!(t.admits(ROOM, &endpoint, 0) == (1000 <= TICKET_SKEW_SECS));
        assert!(!ticket(key(1).ember_id()).admits(ROOM, &endpoint, u64::MAX));
    }

    #[test]
    fn room_names_are_single_line_text_of_at_most_64_bytes() {
        assert!(check_room_name("Friendly matches").is_ok());
        assert!(check_room_name("a").is_ok());
        assert!(check_room_name("two  spaces inside").is_ok());
        assert!(check_room_name(&"a".repeat(64)).is_ok());
        // 16 four-byte characters are 64 bytes.
        assert!(check_room_name(&"\u{1F525}".repeat(16)).is_ok());
        assert!(check_room_name(&"\u{1F525}".repeat(17)).is_err());
        assert!(check_room_name("").is_err());
        assert!(check_room_name(&"a".repeat(65)).is_err());
        assert!(check_room_name("two\nlines").is_err());
        assert!(check_room_name("a\r").is_err());
        assert!(check_room_name("tab\there").is_err());
        assert!(check_room_name("nul\0").is_err());
        assert!(check_room_name(" leading").is_err());
        assert!(check_room_name("trailing ").is_err());
        assert!(check_room_name("   ").is_err());
        assert!(check_room_name("\u{3000}ideographic").is_err());
    }

    #[test]
    fn requests_and_summaries_are_bounded() {
        let create = CreateRoom {
            name: "Room".into(),
            capacity: 8,
            build_id: "build".into(),
        };
        assert!(create.check().is_ok());
        for capacity in [0, 1, 17, 255] {
            let bad = CreateRoom {
                capacity,
                ..create.clone()
            };
            assert_eq!(bad.check(), Err(Error::InvalidField("capacity")));
        }
        for capacity in [2, 16] {
            let ok = CreateRoom {
                capacity,
                ..create.clone()
            };
            assert!(ok.check().is_ok());
        }
        let bad = CreateRoom {
            name: "".into(),
            ..create.clone()
        };
        assert_eq!(bad.check(), Err(Error::InvalidField("name")));
        let bad = CreateRoom {
            build_id: "".into(),
            ..create
        };
        assert_eq!(bad.check(), Err(Error::InvalidField("build_id")));

        let request = TicketRequest {
            endpoint_id: "c".repeat(64),
            build_id: "build".into(),
        };
        assert!(request.check().is_ok());
        let bad = TicketRequest {
            endpoint_id: "c".repeat(63),
            ..request.clone()
        };
        assert!(bad.check().is_err());
        let bad = TicketRequest {
            build_id: "b\u{e9}".into(),
            ..request
        };
        assert!(bad.check().is_err());

        assert!(summary().check().is_ok());
        let full = RoomSummary {
            members: 8,
            ..summary()
        };
        assert!(full.check().is_ok());
        let over = RoomSummary {
            members: 9,
            ..summary()
        };
        assert_eq!(over.check(), Err(Error::InvalidField("members")));
        for region in ["", "USE1", "us-east", "use1use1x", "us e"] {
            let bad = RoomSummary {
                region: region.into(),
                ..summary()
            };
            assert_eq!(bad.check(), Err(Error::InvalidField("region")), "{region}");
        }
        let bad = RoomSummary {
            room_id: "0".repeat(31),
            ..summary()
        };
        assert!(bad.check().is_err());
        let bad = RoomSummary {
            capacity: 1,
            members: 0,
            ..summary()
        };
        assert!(bad.check().is_err());
    }

    #[test]
    fn an_admission_names_one_room() {
        let bridge = key(9);
        let signed = ticket(key(1).ember_id()).sign(&bridge, "k1").unwrap();
        let admission = RoomAdmission {
            room: summary(),
            invitation: "sf4e3:abc".into(),
            ticket: signed,
        };
        assert!(admission.check().is_ok());
        let mut other = admission.clone();
        other.room.room_id = "f".repeat(32);
        assert_eq!(other.check(), Err(Error::Mismatch("room_id")));
        let mut empty = admission;
        empty.invitation.clear();
        assert!(empty.check().is_err());
    }

    #[test]
    fn unknown_json_fields_are_rejected() {
        let good = br#"{"name":"Room","capacity":8,"build_id":"build"}"#;
        let parsed: CreateRoom = json::parse_as(good, 1024).unwrap();
        assert_eq!(parsed.capacity, 8);
        let extra = br#"{"name":"Room","capacity":8,"build_id":"build","owner":"x"}"#;
        assert!(json::parse_as::<CreateRoom>(extra, 1024).is_err());
        let extra = format!(
            r#"{{"endpoint_id":"{}","build_id":"build","moderator":true}}"#,
            "a".repeat(64)
        );
        assert!(json::parse_as::<TicketRequest>(extra.as_bytes(), 1024).is_err());
        let extra = br#"{"rooms":[],"total":0}"#;
        assert!(json::parse_as::<RoomList>(extra, 1024).is_err());
        // A ticket no longer carries a moderator flag.
        let id = key(1).ember_id().clone();
        let mut value = json::to_value(&ticket(&id)).unwrap();
        let text = String::from_utf8(value.canonical()).unwrap();
        let widened = text.replacen('{', r#"{"moderator":true,"#, 1);
        assert!(json::parse_as::<RoomTicket>(widened.as_bytes(), 4096).is_err());
        value = json::parse(text.as_bytes(), 4096).unwrap();
        assert_eq!(json::from_value::<RoomTicket>(&value).unwrap(), ticket(&id));
    }
}
