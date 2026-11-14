//! A public room's area on the wire. A create carries it only to a bridge
//! that lists `AREA_FEATURE`, since a bridge from before it refuses the field,
//! and a listing carries it only for a room whose creator gave one. The
//! relay code keeps travelling in `region` as it did, so a 1.5.7 build, whose
//! reader of a room takes fields it does not know, lists and joins exactly as
//! before. The frozen readers below are the types as they were before areas.
use ember_protocol::{
    area::Area,
    rooms::{AREA_FEATURE, CreateRoom, RoomList, RoomSummary},
};
use serde::Deserialize;

const OLD_SUMMARY: &str = r#"{"room_id":"0123456789abcdef0123456789abcdef","name":"Friendly matches","build_id":"build","members":3,"capacity":8,"tables_playing":1,"region":"use1","created_at":900}"#;
const OLD_CREATE: &str = r#"{"name":"Room","capacity":8,"build_id":"build","region":"use1"}"#;

/// `RoomSummary` as Stable 1.5.7 reads it
/// (`server/ember/protocol/src/rooms/mod.rs` on `release`): a field it does
/// not know is ignored.
#[derive(Debug, Deserialize)]
#[allow(dead_code)]
struct ReleasedRoomSummary {
    room_id: String,
    name: String,
    build_id: String,
    members: u8,
    capacity: u8,
    tables_playing: u8,
    region: String,
    created_at: u64,
    #[serde(default)]
    own_delay: bool,
}

#[derive(Debug, Deserialize)]
#[allow(dead_code)]
struct ReleasedRoomList {
    rooms: Vec<ReleasedRoomSummary>,
    listed_at: Option<u64>,
}

/// `CreateRoom` before `area`, which is what a bridge without
/// `AREA_FEATURE` parses.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
#[allow(dead_code)]
struct OldCreateRoom {
    name: String,
    capacity: u8,
    build_id: String,
    region: Option<String>,
    #[serde(default)]
    own_delay: bool,
}

fn create(area: Option<Area>) -> CreateRoom {
    CreateRoom {
        area,
        ..serde_json::from_str::<CreateRoom>(OLD_CREATE).unwrap()
    }
}

fn summary(area: Option<&str>) -> RoomSummary {
    RoomSummary {
        area: area.map(str::to_owned),
        ..serde_json::from_str::<RoomSummary>(OLD_SUMMARY).unwrap()
    }
}

#[test]
fn the_capability_is_named() {
    assert_eq!(AREA_FEATURE, "room_area");
}

#[test]
fn a_create_with_or_without_an_area_round_trips_and_the_old_shape_still_reads() {
    let old: CreateRoom = serde_json::from_str(OLD_CREATE).unwrap();
    assert_eq!(old.area, None);
    old.check().unwrap();
    assert_eq!(serde_json::to_string(&old).unwrap(), OLD_CREATE);
    for area in Area::ALL {
        let room = create(Some(area));
        room.check().unwrap();
        let text = serde_json::to_string(&room).unwrap();
        assert!(
            text.contains(&format!(r#""area":"{}""#, area.as_str())),
            "{text}"
        );
        assert_eq!(serde_json::from_str::<CreateRoom>(&text).unwrap(), room);
        // The relay code still goes beside it, for routing.
        assert!(text.contains(r#""region":"use1""#), "{text}");
    }
    // A creator who hid their area sends none.
    assert!(
        !serde_json::to_string(&create(None))
            .unwrap()
            .contains("area")
    );
}

#[test]
fn a_create_names_a_known_area_or_none() {
    for area in [r#""""#, r#""use1""#, r#""NAW""#, r#""na-w""#, "7", "null"] {
        let text = OLD_CREATE.replacen('}', &format!(r#","area":{area}}}"#), 1);
        let parsed = serde_json::from_str::<CreateRoom>(&text);
        if area == "null" {
            assert_eq!(parsed.unwrap().area, None);
        } else {
            assert!(parsed.is_err(), "{text}");
        }
    }
}

#[test]
fn a_bridge_from_before_areas_refuses_a_create_with_one_and_takes_one_without() {
    let with = serde_json::to_string(&create(Some(Area::SouthAmerica))).unwrap();
    assert!(serde_json::from_str::<OldCreateRoom>(&with).is_err());
    let without = serde_json::to_string(&create(None)).unwrap();
    assert!(serde_json::from_str::<OldCreateRoom>(&without).is_ok());
}

#[test]
fn a_listing_carries_the_area_only_when_there_is_one_and_a_released_build_reads_it() {
    let plain = summary(None);
    assert_eq!(serde_json::to_string(&plain).unwrap(), OLD_SUMMARY);
    let placed = summary(Some("sam"));
    placed.check().unwrap();
    let text = serde_json::to_string(&placed).unwrap();
    assert!(text.contains(r#""area":"sam""#), "{text}");
    assert_eq!(serde_json::from_str::<RoomSummary>(&text).unwrap(), placed);
    // Stable 1.5.7 reads the room as before: its relay code is all it shows.
    let list = RoomList {
        rooms: vec![placed, plain],
        listed_at: Some(1000),
    };
    let released: ReleasedRoomList =
        serde_json::from_str(&serde_json::to_string(&list).unwrap()).unwrap();
    assert_eq!(released.rooms.len(), 2);
    assert!(released.rooms.iter().all(|room| room.region == "use1"));
}
