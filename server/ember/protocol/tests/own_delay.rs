//! A room's own delay on the public wire. The property is written only when it
//! is set, so every message about a room with the shared delay is what it was
//! before, and a reader that refuses unknown fields (every one of them does)
//! never meets the field. The frozen readers below are the room types as they
//! were before the field existed.
use ember_protocol::{
    SigningIdentity, json,
    play::VERSION,
    rooms::{
        ConnectionCreateRoom, CreateRoom, RoomAdmission, RoomCreator, RoomList, RoomSummary,
        RoomTicket, TICKET_SECS,
    },
};
use serde::Deserialize;
use zeroize::Zeroizing;

const OLD_SUMMARY: &str = r#"{"room_id":"0123456789abcdef0123456789abcdef","name":"Friendly matches","build_id":"build","members":3,"capacity":8,"tables_playing":1,"region":"use1","created_at":900}"#;
const OLD_DETAILED_SUMMARY: &str = r#"{"room_id":"0123456789abcdef0123456789abcdef","name":"Friendly matches","build_id":"build","members":3,"capacity":8,"tables_playing":1,"region":"use1","created_at":900,"host_name":"Kate","fighters":[3,255,0],"locked":false,"set_format":3,"rotation":1}"#;
const OLD_CREATE: &str = r#"{"name":"Room","capacity":8,"build_id":"build","region":"euc1"}"#;
const ROOM: &str = "0123456789abcdef0123456789abcdef";
const BRIDGE: &str = "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11";

/// `RoomSummary` before `own_delay`: unknown fields are refused, as the
/// helper's reader of a whole listing does.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
#[allow(dead_code)]
struct OldRoomSummary {
    room_id: String,
    name: String,
    build_id: String,
    members: u8,
    capacity: u8,
    tables_playing: u8,
    region: String,
    created_at: u64,
    host_name: Option<String>,
    fighters: Option<Vec<u8>>,
    locked: Option<bool>,
    set_format: Option<u8>,
    rotation: Option<u8>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
#[allow(dead_code)]
struct OldRoomList {
    rooms: Vec<OldRoomSummary>,
    listed_at: Option<u64>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
#[allow(dead_code)]
struct OldRoomAdmission {
    room: OldRoomSummary,
    invitation: String,
    ticket: serde_json::Value,
}

/// `CreateRoom` before `own_delay`, which is what an older bridge parses.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
#[allow(dead_code)]
struct OldCreateRoom {
    name: String,
    capacity: u8,
    build_id: String,
    region: Option<String>,
}

fn summary(own_delay: bool) -> RoomSummary {
    let mut room: RoomSummary = serde_json::from_str(OLD_SUMMARY).unwrap();
    room.own_delay = own_delay;
    room
}

fn admission(own_delay: bool) -> RoomAdmission {
    let bridge = SigningIdentity::from_seed(&Zeroizing::new([9; 32])).unwrap();
    let player = SigningIdentity::from_seed(&Zeroizing::new([1; 32])).unwrap();
    let ticket = RoomTicket {
        version: VERSION,
        bridge_id: BRIDGE.into(),
        room_id: ROOM.into(),
        ember_id: player.ember_id().clone(),
        endpoint_id: "a".repeat(64),
        issued_at: 1000,
        expires_at: 1000 + TICKET_SECS,
        badge: None,
    };
    RoomAdmission {
        room: summary(own_delay),
        invitation: "sf4e3:abc".into(),
        ticket: ticket.sign(&bridge, "k1").unwrap(),
    }
}

#[test]
fn a_shared_room_is_listed_exactly_as_before() {
    let room = summary(false);
    assert_eq!(serde_json::to_string(&room).unwrap(), OLD_SUMMARY);
    let detailed: RoomSummary = serde_json::from_str(OLD_DETAILED_SUMMARY).unwrap();
    assert!(!detailed.own_delay && detailed.details_valid());
    assert_eq!(
        serde_json::to_string(&detailed).unwrap(),
        OLD_DETAILED_SUMMARY
    );
    let list = RoomList {
        rooms: vec![room.clone(), detailed],
        listed_at: Some(1234),
    };
    assert!(!serde_json::to_string(&list).unwrap().contains("own_delay"));
    // An explicit false is read as the default and not written back.
    let explicit = OLD_SUMMARY.replacen('}', r#","own_delay":false}"#, 1);
    let parsed: RoomSummary = serde_json::from_str(&explicit).unwrap();
    assert_eq!(parsed, room);
    assert_eq!(serde_json::to_string(&parsed).unwrap(), OLD_SUMMARY);
}

#[test]
fn an_older_reader_parses_a_shared_rooms_listing_and_admission() {
    let list = RoomList {
        rooms: vec![summary(false)],
        listed_at: Some(1234),
    };
    let text = serde_json::to_string(&list).unwrap();
    let old: OldRoomList = serde_json::from_str(&text).unwrap();
    assert_eq!(old.rooms.len(), 1);
    let admission = admission(false);
    admission.check().unwrap();
    let text = serde_json::to_string(&admission).unwrap();
    let old: OldRoomAdmission = serde_json::from_str(&text).unwrap();
    assert_eq!(old.room.room_id, ROOM);
    // The newer reader reads the same bytes back, with the shared delay.
    let back: RoomAdmission = serde_json::from_str(&text).unwrap();
    assert!(!back.room.own_delay);
}

#[test]
fn an_own_delay_room_says_so_in_its_listing_and_admission() {
    let room = summary(true);
    let text = serde_json::to_string(&room).unwrap();
    assert!(text.contains(r#""own_delay":true"#), "{text}");
    assert_eq!(serde_json::from_str::<RoomSummary>(&text).unwrap(), room);
    room.check().unwrap();
    let admission = admission(true);
    admission.check().unwrap();
    let text = serde_json::to_string(&admission).unwrap();
    assert!(text.contains(r#""own_delay":true"#), "{text}");
    assert!(
        serde_json::from_str::<RoomAdmission>(&text)
            .unwrap()
            .room
            .own_delay
    );
    // A reader of the older shape refuses it: such a room is only listed to a
    // client of the build whose room host made it (a listing names a build).
    assert!(
        serde_json::from_str::<OldRoomSummary>(&serde_json::to_string(&room).unwrap()).is_err()
    );
    // Anything but a boolean is not a room.
    let bad = OLD_SUMMARY.replacen('}', r#","own_delay":"yes"}"#, 1);
    assert!(serde_json::from_str::<RoomSummary>(&bad).is_err());
}

#[test]
fn a_request_for_the_shared_delay_is_what_it_was_before() {
    let parsed: CreateRoom = json::parse_as(OLD_CREATE.as_bytes(), 1024).unwrap();
    assert!(!parsed.own_delay);
    assert_eq!(serde_json::to_string(&parsed).unwrap(), OLD_CREATE);
    parsed.check().unwrap();
    let explicit = OLD_CREATE.replacen('}', r#","own_delay":false}"#, 1);
    let parsed: CreateRoom = json::parse_as(explicit.as_bytes(), 1024).unwrap();
    assert_eq!(serde_json::to_string(&parsed).unwrap(), OLD_CREATE);
    // A connection never asks for it: its room is the shared one.
    let player = SigningIdentity::from_seed(&Zeroizing::new([1; 32])).unwrap();
    let connection = ConnectionCreateRoom {
        name: "Room".into(),
        capacity: 8,
        build_id: "build".into(),
        creator: RoomCreator {
            participant_id: "p1".into(),
            ember_id: player.ember_id().clone(),
        },
    };
    assert!(!connection.room().own_delay);
}

#[test]
fn a_request_for_each_players_own_delay_is_refused_by_an_older_bridge() {
    let mut own: CreateRoom = serde_json::from_str(OLD_CREATE).unwrap();
    own.own_delay = true;
    own.check().unwrap();
    let text = serde_json::to_string(&own).unwrap();
    assert!(text.ends_with(r#","own_delay":true}"#), "{text}");
    assert_eq!(
        json::parse_as::<CreateRoom>(text.as_bytes(), 1024).unwrap(),
        own
    );
    // The older bridge's parser refuses the unknown field, so the request fails
    // there instead of making a room with the shared delay.
    assert!(serde_json::from_str::<OldCreateRoom>(&text).is_err());
    assert!(serde_json::from_str::<OldCreateRoom>(OLD_CREATE).is_ok());
    // Anything but a boolean is not a request.
    let bad = OLD_CREATE.replacen('}', r#","own_delay":1}"#, 1);
    assert!(json::parse_as::<CreateRoom>(bad.as_bytes(), 1024).is_err());
}
