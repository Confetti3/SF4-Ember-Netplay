//! A room's Wired players only rule in its listing. The property is written
//! only when it is set, so a room without it is listed exactly as before, and
//! one that has it reads back with it.
use ember_protocol::rooms::{RoomList, RoomSummary};

const DETAILED: &str = r#"{"room_id":"0123456789abcdef0123456789abcdef","name":"Friendly matches","build_id":"build","members":3,"capacity":8,"tables_playing":1,"region":"use1","created_at":900,"host_name":"Player One","fighters":[3,255,0],"locked":false,"set_format":3,"rotation":1,"room_type":0}"#;

#[test]
fn a_room_without_the_rule_is_listed_as_before() {
    let room: RoomSummary = serde_json::from_str(DETAILED).unwrap();
    assert!(!room.wired_only && room.details_valid());
    assert_eq!(serde_json::to_string(&room).unwrap(), DETAILED);
    // An explicit false reads as the default and is not written back.
    let explicit = DETAILED.replacen('}', r#","wired_only":false}"#, 1);
    let parsed: RoomSummary = serde_json::from_str(&explicit).unwrap();
    assert_eq!(parsed, room);
    assert_eq!(serde_json::to_string(&parsed).unwrap(), DETAILED);
}

#[test]
fn a_wired_room_says_so_and_reads_back() {
    let mut room: RoomSummary = serde_json::from_str(DETAILED).unwrap();
    room.wired_only = true;
    room.check().unwrap();
    let text = serde_json::to_string(&room).unwrap();
    assert!(text.ends_with(r#","wired_only":true}"#), "{text}");
    assert_eq!(serde_json::from_str::<RoomSummary>(&text).unwrap(), room);
    let list = RoomList {
        rooms: vec![room.clone()],
        listed_at: Some(1000),
    };
    let back: RoomList = serde_json::from_str(&serde_json::to_string(&list).unwrap()).unwrap();
    assert!(back.rooms[0].wired_only);
    // Not a boolean: the listing does not read.
    let bad = DETAILED.replacen('}', r#","wired_only":1}"#, 1);
    assert!(serde_json::from_str::<RoomSummary>(&bad).is_err());
}
