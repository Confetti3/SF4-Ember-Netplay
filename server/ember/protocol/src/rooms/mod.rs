//! Public rooms: the signed ticket a bridge issues for each join of a listed
//! room, and the objects of the bridge's room routes (see
//! `docs/design/PUBLIC_ROOMS.md`).
//!
//! The ticket is in `ticket`, the routes of a provider connection in
//! `connection`; this module holds what a player's routes carry, and
//! re-exports the other two so every item keeps its path.
//!
//! A ticket is signed with the bridge's own key under its own domain, using
//! the same canonical JSON and Ed25519 rules as bindings and permits. Only a
//! room host verifies it, against the key the bridge gave the supervisor when
//! the room was created.
use serde::{Deserialize, Serialize};

use crate::{
    Error, Result,
    encoding::{is_false, is_single_line},
    matches::games_to_win_valid,
    play::{MAX_INVITATION, check_build, is_hex},
    relay,
};

mod connection;
mod ticket;

pub use connection::{
    CLOSED_BY_CONNECTION, CloseRoom, ConnectionCreateRoom, ConnectionRoom, ConnectionRoomList,
    ENDED, NOT_LINKED, ROOM_PAGE, RoomCreator, RoomState, join_url,
};
pub use ticket::{RoomTicket, SignedRoomTicket};

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
/// A ticket asked for while the room's moderator has the room locked.
pub const ROOM_LOCKED: &str = "room_locked";

/// How long a ticket is good for.
pub const TICKET_SECS: u64 = 60;
/// How far ahead of the verifier's clock a ticket may have been issued.
pub const TICKET_SKEW_SECS: u64 = 30;
pub const MAX_NAME: usize = 64;
pub const MIN_CAPACITY: u8 = 2;
pub const MAX_CAPACITY: u8 = 16;
/// The longest moderator name a listing carries, in bytes.
pub const MAX_HOST_NAME: usize = 32;
/// A fighter slot with no known character.
pub const NO_FIGHTER: u8 = 255;
/// Character IDs below this are fighters; `NO_FIGHTER` is the only other value.
pub const FIGHTER_LIMIT: u8 = 64;
/// The set format of a table that plays on without a set length. Any other
/// format is first to N (`matches::games_to_win_valid`).
pub const UNLIMITED_SET: u8 = 0;
/// Winner stays, loser stays, both rotate.
pub const MAX_ROTATION: u8 = 2;
/// Accounts a server-owned room may ban in its lifetime. Bans are never
/// evicted: the room authority closes the room instead of exceeding this.
/// The same number is in `server/roomhost` (the C++ room model),
/// `server/ember-rooms/src/protocol.rs` (`MAX_ROOM_BANS`, a separate crate) and
/// the helper (`sf4-net` `public_room`, which uses this constant); keep
/// them equal.
pub const MAX_ROOM_BANS: usize = 512;
const MAX_REGION: usize = 8;

/// A room name: 1 to 64 bytes of single-line text with no control characters
/// and no leading or trailing whitespace.
pub fn check_room_name(name: &str) -> Result<()> {
    if is_single_line(name, MAX_NAME) {
        Ok(())
    } else {
        Err(Error::InvalidField("name"))
    }
}

/// A moderator's name, as any player name: `is_player_name`.
pub fn check_host_name(name: &str) -> Result<()> {
    if is_player_name(name) {
        Ok(())
    } else {
        Err(Error::InvalidField("host_name"))
    }
}

/// A country as players show it: an ISO 3166 alpha-2 code in capitals.
pub fn is_country(code: &str) -> bool {
    code.len() == 2 && code.bytes().all(|byte| byte.is_ascii_uppercase())
}

/// A player's name as rooms and the ranking board show it: 1 to 32 bytes of
/// single-line text with no control characters and no leading or trailing
/// whitespace.
pub fn is_player_name(name: &str) -> bool {
    is_single_line(name, MAX_HOST_NAME)
}

/// Whether `format` is one a table can play: unlimited or first to 1 to 10.
pub fn set_format_valid(format: u8) -> bool {
    format == UNLIMITED_SET || games_to_win_valid(format)
}

fn check_capacity(capacity: u8) -> Result<()> {
    if (MIN_CAPACITY..=MAX_CAPACITY).contains(&capacity) {
        Ok(())
    } else {
        Err(Error::InvalidField("capacity"))
    }
}

/// One open room in a listing.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct RoomSummary {
    pub room_id: String,
    pub name: String,
    pub build_id: String,
    pub members: u8,
    pub capacity: u8,
    pub tables_playing: u8,
    /// The room's region code, such as `use1`: its creator's relay region
    /// when they gave one (`CreateRoom::region`), else its host's.
    pub region: String,
    pub created_at: u64,
    /// Each fighter plays at the input delay they chose, instead of both at the
    /// higher of the two. Fixed when the room is created. Present only when
    /// true: a room with the shared delay says nothing, so a reader that
    /// predates the field never sees it.
    #[serde(default, skip_serializing_if = "is_false")]
    pub own_delay: bool,
    /// What follows is the room host's own report and is only in a listing
    /// that asked for it (`detail=1`); a bridge that does not know it, or a
    /// room host that has not said, leaves each one out.
    ///
    /// The moderator's name.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub host_name: Option<String>,
    /// The members' main characters, the moderator first and then in join
    /// order; `NO_FIGHTER` where there is none.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fighters: Option<Vec<u8>>,
    /// The moderator has the room locked: nobody new gets in.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub locked: Option<bool>,
    /// The first table's set format (`set_format_valid`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub set_format: Option<u8>,
    /// The first table's rotation: 0 to `MAX_ROTATION`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rotation: Option<u8>,
}

impl RoomSummary {
    /// The room host's optional report, field by field.
    fn check_details(&self) -> Result<()> {
        if let Some(name) = &self.host_name {
            check_host_name(name)?;
        }
        if let Some(fighters) = &self.fighters
            && (fighters.len() > usize::from(self.members)
                || fighters
                    .iter()
                    .any(|fighter| *fighter >= FIGHTER_LIMIT && *fighter != NO_FIGHTER))
        {
            return Err(Error::InvalidField("fighters"));
        }
        if self
            .set_format
            .is_some_and(|format| !set_format_valid(format))
        {
            return Err(Error::InvalidField("set_format"));
        }
        if self
            .rotation
            .is_some_and(|rotation| rotation > MAX_ROTATION)
        {
            return Err(Error::InvalidField("rotation"));
        }
        Ok(())
    }

    /// Whether the optional fields the room host reported are well formed.
    /// A bridge that finds them not drops all of them from the listing.
    pub fn details_valid(&self) -> bool {
        self.check_details().is_ok()
    }

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
        self.check_details()
    }
}

/// `GET /v1/rooms`: at most 100 open rooms, joinable ones with the most free
/// seats first.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct RoomList {
    pub rooms: Vec<RoomSummary>,
    /// When the bridge made this listing (unix seconds), so a client can age
    /// each room by `listed_at - created_at`. Only with `detail=1`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub listed_at: Option<u64>,
}

/// `POST /v1/rooms`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateRoom {
    pub name: String,
    pub capacity: u8,
    pub build_id: String,
    /// The creator's relay region (`relay::REGIONS`), which the listing shows
    /// in place of the room host's: matches are played player to player.
    /// Left out when the creator's region is not known.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub region: Option<String>,
    /// Asks for a room whose fighters each play at their own input delay (see
    /// `RoomSummary::own_delay`). Sent only when true; a bridge that does not
    /// know the field refuses the request, which is never retried without it.
    #[serde(default, skip_serializing_if = "is_false")]
    pub own_delay: bool,
}

impl CreateRoom {
    pub fn check(&self) -> Result<()> {
        check_room_name(&self.name)?;
        check_capacity(self.capacity)?;
        check_build(&self.build_id)?;
        if self
            .region
            .as_deref()
            .is_some_and(|code| !relay::is_region(code))
        {
            return Err(Error::InvalidField("region"));
        }
        Ok(())
    }
}

/// `POST /v1/rooms/{room_id}/tickets`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TicketRequest {
    pub endpoint_id: String,
    pub build_id: String,
    /// The joiner's build shows skill badges and wants its own in the ticket.
    /// A build that does not ask gets a ticket it can read.
    #[serde(default, skip_serializing_if = "is_false")]
    pub skill_badge: bool,
    /// The joiner's build shows Ember's developer mark in place of a class
    /// (`SkillBadge::developer`). A build sends it only to a bridge that
    /// offers ranking, since an older one refuses the field.
    #[serde(default, skip_serializing_if = "is_false")]
    pub developer_badge: bool,
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

#[cfg(test)]
mod tests {
    use zeroize::Zeroizing;

    use crate::{json, matches::MAX_GAMES_TO_WIN};

    use crate::{EmberId, SigningIdentity, play::VERSION};

    use super::*;

    pub(super) const BRIDGE: &str = "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11";
    pub(super) const ROOM: &str = "0123456789abcdef0123456789abcdef";

    pub(super) fn key(byte: u8) -> SigningIdentity {
        SigningIdentity::from_seed(&Zeroizing::new([byte; 32])).unwrap()
    }

    pub(super) fn ticket(ember_id: &EmberId) -> RoomTicket {
        RoomTicket {
            version: VERSION,
            bridge_id: BRIDGE.into(),
            room_id: ROOM.into(),
            ember_id: ember_id.clone(),
            endpoint_id: "a".repeat(64),
            issued_at: 1000,
            expires_at: 1000 + TICKET_SECS,
            badge: None,
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
            own_delay: false,
            host_name: None,
            fighters: None,
            locked: None,
            set_format: None,
            rotation: None,
        }
    }

    fn detailed() -> RoomSummary {
        RoomSummary {
            host_name: Some("Kate".into()),
            fighters: Some(vec![3, NO_FIGHTER, 0]),
            locked: Some(false),
            set_format: Some(3),
            rotation: Some(1),
            ..summary()
        }
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
            region: None,
            own_delay: false,
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
            ..create.clone()
        };
        assert_eq!(bad.check(), Err(Error::InvalidField("build_id")));
        // A creator's region is one of the known codes or left out.
        for region in relay::REGIONS {
            let ok = CreateRoom {
                region: Some(region.into()),
                ..create.clone()
            };
            assert!(ok.check().is_ok(), "{region}");
        }
        for region in ["", "other", "USE1", "test", "us-east"] {
            let bad = CreateRoom {
                region: Some(region.into()),
                ..create.clone()
            };
            assert_eq!(bad.check(), Err(Error::InvalidField("region")), "{region}");
        }

        let request = TicketRequest {
            endpoint_id: "c".repeat(64),
            build_id: "build".into(),
            skill_badge: false,
            developer_badge: false,
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
    fn the_old_summary_shape_still_parses_and_serializes_unchanged() {
        let old = r#"{"room_id":"0123456789abcdef0123456789abcdef","name":"Friendly matches","build_id":"build","members":3,"capacity":8,"tables_playing":1,"region":"use1","created_at":900}"#;
        let parsed: RoomSummary = serde_json::from_str(old).unwrap();
        assert_eq!(parsed, summary());
        parsed.check().unwrap();
        assert_eq!(serde_json::to_string(&parsed).unwrap(), old);
        let old_list = format!(r#"{{"rooms":[{old}]}}"#);
        let list: RoomList = serde_json::from_str(&old_list).unwrap();
        assert_eq!((list.rooms.len(), list.listed_at), (1, None));
        assert_eq!(serde_json::to_string(&list).unwrap(), old_list);
    }

    #[test]
    fn the_detailed_shape_round_trips() {
        let room = detailed();
        room.check().unwrap();
        assert!(room.details_valid());
        let text = serde_json::to_string(&room).unwrap();
        assert!(text.contains(r#""fighters":[3,255,0]"#), "{text}");
        assert!(text.contains(r#""locked":false"#), "{text}");
        assert_eq!(serde_json::from_str::<RoomSummary>(&text).unwrap(), room);
        let list = RoomList {
            rooms: vec![room],
            listed_at: Some(1234),
        };
        let text = serde_json::to_string(&list).unwrap();
        assert!(text.ends_with(r#""listed_at":1234}"#), "{text}");
        assert_eq!(serde_json::from_str::<RoomList>(&text).unwrap(), list);
        // An answer read by clients ignores a field it does not know.
        let extra = text.replacen('{', r#"{"total":1,"#, 1);
        assert_eq!(serde_json::from_str::<RoomList>(&extra).unwrap(), list);
    }

    #[test]
    fn each_invalid_detail_fails_the_summary() {
        let bad = [
            (
                "host_name",
                RoomSummary {
                    host_name: Some("".into()),
                    ..detailed()
                },
            ),
            (
                "host_name",
                RoomSummary {
                    host_name: Some(" Kate".into()),
                    ..detailed()
                },
            ),
            (
                "host_name",
                RoomSummary {
                    host_name: Some(
                        "Ka
te"
                        .into(),
                    ),
                    ..detailed()
                },
            ),
            (
                "host_name",
                RoomSummary {
                    host_name: Some("k".repeat(33)),
                    ..detailed()
                },
            ),
            (
                "fighters",
                RoomSummary {
                    fighters: Some(vec![64]),
                    ..detailed()
                },
            ),
            (
                "fighters",
                RoomSummary {
                    fighters: Some(vec![254]),
                    ..detailed()
                },
            ),
            (
                "fighters",
                RoomSummary {
                    fighters: Some(vec![1; 4]),
                    ..detailed()
                },
            ),
            (
                "set_format",
                RoomSummary {
                    set_format: Some(11),
                    ..detailed()
                },
            ),
            (
                "set_format",
                RoomSummary {
                    set_format: Some(255),
                    ..detailed()
                },
            ),
            (
                "rotation",
                RoomSummary {
                    rotation: Some(3),
                    ..detailed()
                },
            ),
        ];
        for (field, room) in bad {
            assert_eq!(room.check(), Err(Error::InvalidField(field)), "{room:?}");
            assert!(!room.details_valid(), "{room:?}");
        }
        for format in 0..=MAX_GAMES_TO_WIN {
            let room = RoomSummary {
                set_format: Some(format),
                ..detailed()
            };
            assert!(room.check().is_ok(), "{format}");
        }
        let edge = RoomSummary {
            host_name: Some("k".repeat(32)),
            fighters: Some(vec![63, 0, 255]),
            rotation: Some(2),
            ..detailed()
        };
        assert!(edge.check().is_ok());
        let empty = RoomSummary {
            fighters: Some(Vec::new()),
            ..detailed()
        };
        assert!(empty.check().is_ok());
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
        assert_eq!((parsed.capacity, parsed.region.as_deref()), (8, None));
        // A request without a region is sent as it always was.
        assert_eq!(serde_json::to_vec(&parsed).unwrap(), good);
        let regional = br#"{"name":"Room","capacity":8,"build_id":"build","region":"euc1"}"#;
        let parsed: CreateRoom = json::parse_as(regional, 1024).unwrap();
        assert_eq!(parsed.region.as_deref(), Some("euc1"));
        assert_eq!(serde_json::to_vec(&parsed).unwrap(), regional);
        let extra = br#"{"name":"Room","capacity":8,"build_id":"build","owner":"x"}"#;
        assert!(json::parse_as::<CreateRoom>(extra, 1024).is_err());
        let extra = format!(
            r#"{{"endpoint_id":"{}","build_id":"build","moderator":true}}"#,
            "a".repeat(64)
        );
        assert!(json::parse_as::<TicketRequest>(extra.as_bytes(), 1024).is_err());
        // A listing is an answer: an unknown field is ignored, not refused.
        let extra = br#"{"rooms":[],"total":0}"#;
        assert!(
            json::parse_as::<RoomList>(extra, 1024)
                .unwrap()
                .rooms
                .is_empty()
        );
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
