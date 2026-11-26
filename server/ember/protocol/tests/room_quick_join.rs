//! A room's classes and waiting players on the wire. A listing carries them
//! only for a room whose host said them, so a room without them is listed
//! exactly as before, and a reader that predates them ignores them.
use ember_protocol::{
    rooms::{MAX_WAITING, RoomList, RoomSummary, WaitingPlayer},
    skill::SKILL_BANDS,
};
use serde::Deserialize;

const OLD_SUMMARY: &str = r#"{"room_id":"0123456789abcdef0123456789abcdef","name":"Friendly matches","build_id":"build","members":3,"capacity":8,"tables_playing":1,"region":"use1","created_at":900}"#;

/// `RoomSummary` as Stable 1.6.0 reads it: a field it does not know is
/// ignored.
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
    host_name: Option<String>,
    fighters: Option<Vec<u8>>,
}

fn old() -> RoomSummary {
    serde_json::from_str(OLD_SUMMARY).unwrap()
}

fn waiting(class: u8, area: Option<&str>) -> WaitingPlayer {
    WaitingPlayer {
        class,
        area: area.map(str::to_owned),
    }
}

fn with(classes: Option<Vec<u8>>, waiting: Option<Vec<WaitingPlayer>>) -> RoomSummary {
    RoomSummary {
        classes,
        waiting,
        ..old()
    }
}

#[test]
fn a_room_without_them_is_listed_exactly_as_before() {
    let room = old();
    assert_eq!((&room.classes, &room.waiting), (&None, &None));
    room.check().unwrap();
    assert_eq!(serde_json::to_string(&room).unwrap(), OLD_SUMMARY);
}

#[test]
fn classes_and_waiting_players_round_trip_and_a_released_build_still_reads_the_room() {
    let room = with(
        Some(vec![0, 3, 5]),
        Some(vec![waiting(5, Some("nae")), waiting(0, None)]),
    );
    room.check().unwrap();
    let text = serde_json::to_string(&room).unwrap();
    assert!(text.contains(r#""classes":[0,3,5]"#), "{text}");
    assert!(
        text.contains(r#""waiting":[{"class":5,"area":"nae"},{"class":0}]"#),
        "{text}"
    );
    assert_eq!(serde_json::from_str::<RoomSummary>(&text).unwrap(), room);
    let list = RoomList {
        rooms: vec![room, old()],
        listed_at: Some(1000),
    };
    let released: Vec<ReleasedRoomSummary> =
        serde_json::from_value(serde_json::to_value(&list).unwrap()["rooms"].take()).unwrap();
    assert_eq!(released.len(), 2);
    assert!(released.iter().all(|room| room.members == 3));
}

#[test]
fn each_field_is_skipped_alone_and_an_empty_one_is_kept() {
    let classes = serde_json::to_string(&with(Some(vec![4]), None)).unwrap();
    assert!(classes.contains("classes") && !classes.contains("waiting"));
    let waiting_only = serde_json::to_string(&with(None, Some(vec![waiting(1, None)]))).unwrap();
    assert!(waiting_only.contains("waiting") && !waiting_only.contains("classes"));
    let empty = with(Some(Vec::new()), Some(Vec::new()));
    empty.check().unwrap();
    let text = serde_json::to_string(&empty).unwrap();
    assert!(text.contains(r#""classes":[]"#) && text.contains(r#""waiting":[]"#));
}

#[test]
fn classes_are_held_to_the_members_and_the_class_bound() {
    // The room has 3 members.
    assert!(with(Some(vec![SKILL_BANDS; 3]), None).check().is_ok());
    assert!(with(Some(vec![0, 0, 0]), None).check().is_ok());
    assert!(with(Some(vec![1; 4]), None).check().is_err());
    assert!(with(Some(vec![SKILL_BANDS + 1]), None).check().is_err());
    assert!(with(Some(vec![255]), None).check().is_err());
    let error = with(Some(vec![11]), None).check().unwrap_err();
    assert!(error.to_string().contains("classes"), "{error}");
}

#[test]
fn waiting_players_are_held_to_the_members_the_limit_and_the_class_and_area_rules() {
    let some = |count: usize| vec![waiting(2, Some("euw")); count];
    assert!(with(None, Some(some(3))).check().is_ok());
    assert!(with(None, Some(some(4))).check().is_err());
    // More members than the limit still list at most `MAX_WAITING`.
    let crowd = RoomSummary {
        members: 12,
        capacity: 16,
        waiting: Some(some(MAX_WAITING + 1)),
        ..old()
    };
    assert!(crowd.check().is_err());
    let full = RoomSummary {
        members: 12,
        capacity: 16,
        waiting: Some(some(MAX_WAITING)),
        ..old()
    };
    assert!(full.check().is_ok());
    assert!(
        with(None, Some(vec![waiting(SKILL_BANDS, None)]))
            .check()
            .is_ok()
    );
    assert!(
        with(None, Some(vec![waiting(SKILL_BANDS + 1, None)]))
            .check()
            .is_err()
    );
    for area in ["", "NAE", "na-e", "toolong99", "n ae"] {
        let room = with(None, Some(vec![waiting(1, Some(area))]));
        let error = room.check().unwrap_err();
        assert!(error.to_string().contains("waiting"), "{area}: {error}");
    }
    assert!(
        with(None, Some(vec![waiting(1, Some("a1"))]))
            .check()
            .is_ok()
    );
}

#[test]
fn a_waiting_player_without_a_class_does_not_parse() {
    let text = OLD_SUMMARY.replacen('}', r#","waiting":[{"area":"nae"}]}"#, 1);
    assert!(serde_json::from_str::<RoomSummary>(&text).is_err());
}
