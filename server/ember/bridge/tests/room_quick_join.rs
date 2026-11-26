//! Who is in a room, for Quick join: the classes of its members and the
//! players waiting to be met. They reach a player's detailed listing only,
//! are read from every poll (so a room whose report did not change keeps
//! them), are read field by field, and follow whether classes are shown. The
//! poll stores both with the room's other details, and a listing reads the row.
mod common;

use common::{
    Bridge,
    public_rooms::{open_room, player, raw_listing, start, start_showing_classes},
    supervisor::{Supervisor, report, report_details},
};
use ember_protocol::rooms::RoomList;
use serde_json::{Value as Json_, json};

/// What a room host that knows both fields says for a room of three.
fn quick_details() -> Json_ {
    json!({ "classes": [5, 0, 3],
        "waiting": [{ "class": 6, "area": "nae" }, { "class": 0 }, { "class": 9, "area": "euw" }] })
}

/// A room of three members whose host said `details`, polled once.
async fn room_saying(bridge: &Bridge, fake: &Supervisor, details: Json_) -> String {
    let kate = player(bridge, 1).await;
    let room_id = open_room(bridge, &kate, "198.51.100.1", 8).await;
    report(fake, &room_id, 3, 0, &[]);
    report_details(fake, &room_id, details);
    ember_bridge::poll_rooms(bridge.state()).await;
    room_id
}

async fn public_rooms(bridge: &Bridge) -> Json_ {
    let request = bridge.client.get(bridge.url("/v1/rooms/public")).send();
    let (status, body) = common::read(request.await.unwrap()).await;
    assert_eq!(status, reqwest::StatusCode::OK, "{body}");
    body["rooms"].clone()
}

#[tokio::test]
async fn a_detailed_listing_carries_the_classes_sorted_and_the_waiting_players_in_order() {
    let (bridge, fake) = start_showing_classes().await;
    room_saying(&bridge, &fake, quick_details()).await;
    let sam = player(&bridge, 2).await;
    let detailed = raw_listing(&bridge, &sam, "?detail=1").await;
    let room = &detailed["rooms"][0];
    assert_eq!(room["classes"], json!([0, 3, 5]));
    assert_eq!(
        room["waiting"],
        json!([{ "class": 6, "area": "nae" }, { "class": 0 }, { "class": 9, "area": "euw" }])
    );
    let list: RoomList = serde_json::from_value(detailed).unwrap();
    list.rooms[0].check().unwrap();
}

/// The stored `classes` and `waiting` of a room, as bytes.
async fn stored(bridge: &Bridge, room_id: &str) -> (Option<Vec<u8>>, Option<Vec<u8>>) {
    let room_id = room_id.to_owned();
    bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT classes, waiting FROM rooms WHERE room_id = ?1",
                [room_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?)
        })
        .await
        .unwrap()
}

#[tokio::test]
async fn a_listing_reads_what_a_poll_stored_in_the_rooms_row() {
    let (bridge, fake) = start_showing_classes().await;
    let room_id = room_saying(&bridge, &fake, quick_details()).await;
    // One byte per member, ascending; per waiting player their class, the
    // length of their area and the area.
    assert_eq!(
        stored(&bridge, &room_id).await,
        (
            Some(vec![0, 3, 5]),
            Some(vec![6, 3, b'n', b'a', b'e', 0, 0, 9, 3, b'e', b'u', b'w'])
        )
    );
    // No poll runs between this write and the listings: they read the row.
    let id = room_id.clone();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE rooms SET classes = x'0102', waiting = x'0403736173' WHERE room_id = ?1",
                [id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    let sam = player(&bridge, 2).await;
    let room = &raw_listing(&bridge, &sam, "?detail=1").await["rooms"][0];
    assert_eq!(room["classes"], json!([1, 2]));
    assert_eq!(room["waiting"], json!([{ "class": 4, "area": "sas" }]));
    // A row the poll wrote nothing into lists neither.
    let id = room_id.clone();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE rooms SET classes = NULL, waiting = NULL WHERE room_id = ?1",
                [id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    let room = &raw_listing(&bridge, &sam, "?detail=1").await["rooms"][0];
    assert!(
        room.get("classes").is_none() && room.get("waiting").is_none(),
        "{room}"
    );
    // The host's next report writes them again.
    report_details(&fake, &room_id, quick_details());
    ember_bridge::poll_rooms(bridge.state()).await;
    let room = &raw_listing(&bridge, &sam, "?detail=1").await["rooms"][0];
    assert_eq!(room["classes"], json!([0, 3, 5]));
}

#[tokio::test]
async fn a_room_whose_report_did_not_change_keeps_what_it_said() {
    let (bridge, fake) = start_showing_classes().await;
    room_saying(&bridge, &fake, quick_details()).await;
    let sam = player(&bridge, 2).await;
    // The supervisor's rev is the same, so the room is not applied again.
    for _ in 0..2 {
        ember_bridge::poll_rooms(bridge.state()).await;
        let room = &raw_listing(&bridge, &sam, "?detail=1").await["rooms"][0];
        assert_eq!(room["classes"], json!([0, 3, 5]));
        assert_eq!(room["waiting"].as_array().unwrap().len(), 3);
    }
    // The host's next report replaces them, and one that leaves them out
    // clears them.
    let room_id = raw_listing(&bridge, &sam, "?detail=1").await["rooms"][0]["room_id"]
        .as_str()
        .unwrap()
        .to_owned();
    report_details(&fake, &room_id, json!({ "classes": [1, 1, 1] }));
    ember_bridge::poll_rooms(bridge.state()).await;
    let room = &raw_listing(&bridge, &sam, "?detail=1").await["rooms"][0];
    assert_eq!(room["classes"], json!([1, 1, 1]));
    assert!(room.get("waiting").is_none(), "{room}");
    report_details(&fake, &room_id, json!({ "host_name": "Kate" }));
    ember_bridge::poll_rooms(bridge.state()).await;
    let room = &raw_listing(&bridge, &sam, "?detail=1").await["rooms"][0];
    assert!(
        room.get("classes").is_none() && room.get("waiting").is_none(),
        "{room}"
    );
    assert_eq!(room["host_name"], "Kate");
}

#[tokio::test]
async fn classes_that_are_not_shown_leave_no_classes_and_list_every_waiting_class_as_zero() {
    let (bridge, fake) = start().await;
    room_saying(&bridge, &fake, quick_details()).await;
    let sam = player(&bridge, 2).await;
    let detailed = raw_listing(&bridge, &sam, "?detail=1").await;
    let room = &detailed["rooms"][0];
    assert!(room.get("classes").is_none(), "{room}");
    assert_eq!(
        room["waiting"],
        json!([{ "class": 0, "area": "nae" }, { "class": 0 }, { "class": 0, "area": "euw" }])
    );
    let list: RoomList = serde_json::from_value(detailed).unwrap();
    list.rooms[0].check().unwrap();
}

#[tokio::test]
async fn the_plain_listing_and_the_web_page_carry_neither() {
    let (bridge, fake) = start_showing_classes().await;
    room_saying(&bridge, &fake, quick_details()).await;
    let sam = player(&bridge, 2).await;
    for query in ["", "?detail=0"] {
        let room = &raw_listing(&bridge, &sam, query).await["rooms"][0];
        assert!(
            room.get("classes").is_none() && room.get("waiting").is_none(),
            "{room}"
        );
    }
    let public = public_rooms(&bridge).await;
    let room = &public[0];
    assert!(
        room.get("classes").is_none() && room.get("waiting").is_none(),
        "{room}"
    );
    // The rest of the report is still there, as the web page has always had it.
    let detailed = raw_listing(&bridge, &sam, "?detail=1").await;
    let mut expected = detailed["rooms"][0].clone();
    for key in ["classes", "waiting"] {
        expected.as_object_mut().unwrap().remove(key);
    }
    assert_eq!(*room, expected);
}

#[tokio::test]
async fn a_malformed_waiting_list_drops_only_that_field() {
    let (bridge, fake) = start_showing_classes().await;
    room_saying(
        &bridge,
        &fake,
        json!({ "classes": [2, 4], "host_name": "Kate",
            "waiting": [{ "class": 3, "area": "nae" }, { "class": 11 }] }),
    )
    .await;
    let sam = player(&bridge, 2).await;
    let room = &raw_listing(&bridge, &sam, "?detail=1").await["rooms"][0];
    assert_eq!(room["classes"], json!([2, 4]));
    assert_eq!(room["host_name"], "Kate");
    assert!(room.get("waiting").is_none(), "{room}");
}

#[tokio::test]
async fn a_malformed_class_list_drops_only_that_field_and_an_unlisted_area_only_the_area() {
    let (bridge, fake) = start_showing_classes().await;
    room_saying(
        &bridge,
        &fake,
        json!({ "classes": [2, 4, 12, 1], "host_name": "Kate",
            "waiting": [{ "class": 3, "area": "NA East" }, { "class": 4, "area": "nae" }] }),
    )
    .await;
    let sam = player(&bridge, 2).await;
    let room = &raw_listing(&bridge, &sam, "?detail=1").await["rooms"][0];
    assert!(room.get("classes").is_none(), "{room}");
    assert_eq!(room["host_name"], "Kate");
    assert_eq!(
        room["waiting"],
        json!([{ "class": 3 }, { "class": 4, "area": "nae" }])
    );
}

#[tokio::test]
async fn more_classes_than_members_leave_the_room_listed_as_it_would_be_without_a_report() {
    let (bridge, fake) = start_showing_classes().await;
    // Three members, four classes: not valid, and no waiting list is said.
    room_saying(
        &bridge,
        &fake,
        json!({ "classes": [1, 2, 3, 4], "host_name": "Kate" }),
    )
    .await;
    let sam = player(&bridge, 2).await;
    let room = &raw_listing(&bridge, &sam, "?detail=1").await["rooms"][0];
    assert!(room.get("classes").is_none(), "{room}");
    assert_eq!(room["host_name"], "Kate");
}
