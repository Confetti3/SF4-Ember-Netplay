//! Room details: what a room host reports (name, capacity, host name, fighters,
//! lock, rules) reaches the listing only when asked for, is stored leniently,
//! moves the full check, and never reaches the creation or admission views.
mod common;

use common::{
    Bridge,
    public_rooms::{create_room, listing, open_room, player, raw_listing, reason, start, ticket},
    supervisor::{BUILD, report, report_details},
};
use ember_protocol::rooms::{RoomAdmission, RoomList, RoomSummary};
use reqwest::StatusCode;
use serde_json::{Value as Json_, json};

/// What a room host with a full report says.
fn full_details() -> Json_ {
    json!({ "name": "Late night", "capacity": 6, "locked": false, "host_name": "Kate",
        "fighters": [3, 255], "set_format": 3, "rotation": 1 })
}

#[tokio::test]
async fn details_are_listed_with_detail_one_and_absent_without() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    // The clock is the wall clock, so times are checked against a window: a
    // slow run can cross a second between creating the room and reading it.
    let opened = bridge.clock.now();
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 0, &[]);
    report_details(&fake, &room_id, full_details());
    ember_bridge::poll_rooms(bridge.state()).await;

    // Without `detail=1` the answer is exactly what it was before details.
    let plain = raw_listing(&bridge, &sam, "").await;
    let created = plain["rooms"][0]["created_at"].as_u64().unwrap();
    assert!((opened..=bridge.clock.now()).contains(&created), "{plain}");
    assert_eq!(
        plain,
        json!({ "rooms": [{
            "room_id": room_id, "name": "Late night", "build_id": BUILD, "members": 2,
            "capacity": 6, "tables_playing": 0, "region": "use1",
            "created_at": created,
        }] })
    );
    for query in ["?detail=0", "?detail=2"] {
        assert_eq!(raw_listing(&bridge, &sam, query).await, plain, "{query}");
    }

    let asked = bridge.clock.now();
    let detailed = raw_listing(&bridge, &sam, "?detail=1").await;
    let listed = detailed["listed_at"].as_u64().unwrap();
    assert!((asked..=bridge.clock.now()).contains(&listed), "{detailed}");
    let room = &detailed["rooms"][0];
    assert_eq!(room["host_name"], "Kate");
    assert_eq!(room["fighters"], json!([3, 255]));
    assert_eq!(room["locked"], false);
    assert_eq!(
        (&room["set_format"], &room["rotation"]),
        (&json!(3), &json!(1))
    );
    let list: RoomList = serde_json::from_value(detailed).unwrap();
    list.rooms[0].check().unwrap();

    // A room whose host has said nothing carries no extras, even asked for.
    let other = player(&bridge, 3).await;
    let quiet = open_room(&bridge, &other, "198.51.100.3", 4).await;
    report(&fake, &quiet, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let detailed = raw_listing(&bridge, &sam, "?detail=1").await;
    let quiet_room = detailed["rooms"]
        .as_array()
        .unwrap()
        .iter()
        .find(|room| room["room_id"] == quiet.as_str())
        .unwrap();
    for key in ["host_name", "fighters", "locked", "set_format", "rotation"] {
        assert!(quiet_room.get(key).is_none(), "{key}");
    }
}

/// The stored details of a room, as JSON: name, capacity, host name, whether
/// fighters is NULL, locked, set format, rotation.
async fn stored(bridge: &Bridge, room_id: &str) -> Json_ {
    let room_id = room_id.to_owned();
    bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT name, capacity, host_name, fighters IS NULL, locked, set_format, rotation
                 FROM rooms WHERE room_id = ?1",
                [room_id],
                |row| {
                    Ok(json!([
                        row.get::<_, String>(0)?,
                        row.get::<_, u8>(1)?,
                        row.get::<_, Option<String>>(2)?,
                        row.get::<_, bool>(3)?,
                        row.get::<_, Option<bool>>(4)?,
                        row.get::<_, Option<u8>>(5)?,
                        row.get::<_, Option<u8>>(6)?,
                    ]))
                },
            )?)
        })
        .await
        .unwrap()
}

#[tokio::test]
async fn invalid_details_are_stored_as_nothing_and_the_list_stays_valid() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 4).await;
    report(&fake, &room_id, 2, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;

    // Everything wrong: nothing is stored, the name and capacity stay, and the
    // poll and the listing still work.
    for bad in [
        json!({ "name": "two\nlines", "capacity": 1, "locked": "yes", "host_name": "",
            "fighters": [1, 2, 3], "set_format": 11, "rotation": 9 }),
        json!({ "capacity": 1, "fighters": [64] }),
        json!("not an object"),
        json!([1, 2]),
        json!(null),
    ] {
        report_details(&fake, &room_id, bad.clone());
        ember_bridge::poll_rooms(bridge.state()).await;
        assert_eq!(
            stored(&bridge, &room_id).await,
            json!(["Friendly matches", 4, null, true, null, null, null]),
            "{bad}"
        );
        let listed = raw_listing(&bridge, &sam, "?detail=1").await;
        assert_eq!(listed["rooms"][0]["members"], 2);
        assert!(listed["rooms"][0].get("fighters").is_none(), "{bad}");
    }

    // One bad field costs only that field.
    report_details(
        &fake,
        &room_id,
        json!({ "name": "Kept", "capacity": 99, "host_name": "Kate", "fighters": [0, 1, 2],
            "set_format": 2, "rotation": 5, "locked": true }),
    );
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(
        stored(&bridge, &room_id).await,
        json!(["Kept", 4, "Kate", true, true, 2, null])
    );

    // Fighters that no longer fit the members make the listing drop every
    // extra instead of failing.
    report_details(&fake, &room_id, json!({ "fighters": [1, 2] }));
    ember_bridge::poll_rooms(bridge.state()).await;
    report(&fake, &room_id, 1, 0, &[]);
    // The room host says nothing this time, so the stored fighters stay.
    fake.lock().unwrap().rooms[0]
        .as_object_mut()
        .unwrap()
        .remove("details");
    ember_bridge::poll_rooms(bridge.state()).await;
    let listed = raw_listing(&bridge, &sam, "?detail=1").await;
    let room = &listed["rooms"][0];
    assert_eq!(room["members"], 1);
    assert_eq!(room["name"], "Kept");
    for key in ["host_name", "fighters", "locked", "set_format", "rotation"] {
        assert!(room.get(key).is_none(), "{key}");
    }
}

#[tokio::test]
async fn a_rename_and_a_new_capacity_reach_the_listing_and_the_full_check() {
    let (bridge, fake) = start().await;
    let (kate, sam, kim) = (
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
    );
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 4).await;
    report(&fake, &room_id, 2, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;

    // The moderator shrinks the room to the members it has: it is full.
    report_details(&fake, &room_id, json!({ "name": "Tiny", "capacity": 2 }));
    ember_bridge::poll_rooms(bridge.state()).await;
    let rooms = listing(&bridge, &sam, "").await;
    assert_eq!((rooms[0].name.as_str(), rooms[0].capacity), ("Tiny", 2));
    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CONFLICT, "{body}");
    assert_eq!(reason(&body), "room_full");

    // Then grows it past the one it was created with; the members the
    // supervisor reports are no longer cut to the old capacity.
    report_details(&fake, &room_id, json!({ "name": "Big", "capacity": 8 }));
    report(&fake, &room_id, 7, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let rooms = listing(&bridge, &sam, "").await;
    assert_eq!(
        (rooms[0].name.as_str(), rooms[0].capacity, rooms[0].members),
        ("Big", 8, 7)
    );
    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    let admission: RoomAdmission = serde_json::from_value(body).unwrap();
    assert_eq!(
        (admission.room.name.as_str(), admission.room.capacity),
        ("Big", 8)
    );
    report(&fake, &room_id, 8, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let (status, body) = ticket(&bridge, &kim, "198.51.100.3", &room_id, BUILD).await;
    assert_eq!(
        (status, reason(&body)),
        (StatusCode::CONFLICT, "room_full"),
        "{body}"
    );

    // A capacity below the members the same report gives is not taken.
    report_details(&fake, &room_id, json!({ "capacity": 4 }));
    ember_bridge::poll_rooms(bridge.state()).await;
    let rooms = listing(&bridge, &sam, "").await;
    assert_eq!((rooms[0].capacity, rooms[0].members), (8, 8));
}

// A table that moves to a longer set shows it: first to 10 replaces first to 2.
#[tokio::test]
async fn a_longer_set_reaches_the_listing() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 0, &[]);
    for length in [2, 10] {
        report_details(
            &fake,
            &room_id,
            json!({ "set_format": length, "rotation": 0 }),
        );
        ember_bridge::poll_rooms(bridge.state()).await;
        let detailed = raw_listing(&bridge, &sam, "?detail=1").await;
        assert_eq!(detailed["rooms"][0]["set_format"], length, "{detailed}");
    }
}

// The member count and the fighters are judged against the capacity the room
// ends up with, not the one it had: growing from 2 to 8 in one report keeps
// the seven members and their seven fighters.
#[tokio::test]
async fn a_room_growing_with_its_members_keeps_their_fighters() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 4).await;
    report(&fake, &room_id, 2, 0, &[]);
    report_details(&fake, &room_id, json!({ "capacity": 2 }));
    ember_bridge::poll_rooms(bridge.state()).await;

    report(&fake, &room_id, 7, 0, &[]);
    report_details(
        &fake,
        &room_id,
        json!({ "capacity": 8, "fighters": [3, 255, 0, 1, 2, 4, 5], "host_name": "Kate" }),
    );
    ember_bridge::poll_rooms(bridge.state()).await;
    let listed = raw_listing(&bridge, &sam, "?detail=1").await;
    let room = &listed["rooms"][0];
    assert_eq!(
        (&room["capacity"], &room["members"]),
        (&json!(8), &json!(7))
    );
    assert_eq!(room["fighters"], json!([3, 255, 0, 1, 2, 4, 5]));
}

// A capacity below the members the same report gives is refused whatever
// capacity the room had: the old one stays, and the members and fighters are
// held to it.
#[tokio::test]
async fn a_capacity_below_the_reported_members_keeps_the_old_one() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 4).await;
    report(&fake, &room_id, 2, 0, &[]);
    report_details(&fake, &room_id, json!({ "capacity": 2 }));
    ember_bridge::poll_rooms(bridge.state()).await;

    report(&fake, &room_id, 7, 0, &[]);
    report_details(
        &fake,
        &room_id,
        json!({ "capacity": 4, "fighters": [3, 255, 0, 1] }),
    );
    ember_bridge::poll_rooms(bridge.state()).await;
    let listed = raw_listing(&bridge, &sam, "?detail=1").await;
    let room = &listed["rooms"][0];
    assert_eq!(
        (&room["capacity"], &room["members"]),
        (&json!(2), &json!(2))
    );
    assert!(room.get("fighters").is_none(), "{room}");
    assert_eq!(
        stored(&bridge, &room_id).await,
        json!(["Friendly matches", 2, null, true, null, null, null])
    );
}

#[tokio::test]
async fn locked_rooms_are_listed_last_and_refuse_tickets() {
    let (bridge, fake) = start().await;
    let (kate, sam, kim, lee) = (
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
        player(&bridge, 4).await,
    );
    // The locked room has the most free seats, so it would lead otherwise.
    let locked = open_room(&bridge, &kate, "198.51.100.1", 16).await;
    let open = open_room(&bridge, &sam, "198.51.100.2", 4).await;
    for id in [&locked, &open] {
        report(&fake, id, 1, 0, &[]);
    }
    report_details(&fake, &locked, json!({ "locked": true }));
    report_details(&fake, &open, json!({ "locked": false }));
    ember_bridge::poll_rooms(bridge.state()).await;
    let order = |rooms: Vec<RoomSummary>| -> Vec<String> {
        rooms.into_iter().map(|room| room.room_id).collect()
    };
    assert_eq!(
        order(listing(&bridge, &lee, "").await),
        [open.clone(), locked.clone()]
    );
    let detailed = listing(&bridge, &lee, "?detail=1").await;
    assert_eq!(detailed[1].locked, Some(true));
    assert_eq!(detailed[0].locked, Some(false));

    let (status, body) = ticket(&bridge, &kim, "198.51.100.3", &locked, BUILD).await;
    assert_eq!(status, StatusCode::CONFLICT, "{body}");
    assert_eq!(reason(&body), "room_locked");
    assert_eq!(body["error"]["message"], "The room is locked.");
    let (status, _) = ticket(&bridge, &kim, "198.51.100.3", &open, BUILD).await;
    assert_eq!(status, StatusCode::CREATED);

    // The moderator unlocks it: it takes its place by free seats again.
    report_details(&fake, &locked, json!({ "locked": false }));
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(
        order(listing(&bridge, &lee, "").await),
        [locked.clone(), open]
    );
    let (status, _) = ticket(&bridge, &kim, "198.51.100.3", &locked, BUILD).await;
    assert_eq!(status, StatusCode::CREATED);
}

#[tokio::test]
async fn the_creation_and_admission_views_carry_no_details() {
    let (bridge, fake) = start().await;
    let (kate, sam) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let opened = bridge.clock.now();
    let room_id = open_room(&bridge, &kate, "198.51.100.1", 8).await;
    report(&fake, &room_id, 1, 0, &[]);
    report_details(&fake, &room_id, full_details());
    ember_bridge::poll_rooms(bridge.state()).await;
    let (status, body) = ticket(&bridge, &sam, "198.51.100.2", &room_id, BUILD).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    // The admission's room is the plain summary, with the name and capacity
    // the moderator set. The clock is the wall clock, so the creation time is
    // checked against a window.
    let created = body["room"]["created_at"].as_u64().unwrap();
    assert!((opened..=bridge.clock.now()).contains(&created), "{body}");
    assert_eq!(
        body["room"],
        json!({
            "room_id": room_id, "name": "Late night", "build_id": BUILD, "members": 1,
            "capacity": 6, "tables_playing": 0, "region": "use1",
            "created_at": created,
        })
    );
    // Creating another room answers the plain summary too.
    let (status, created) = create_room(&bridge, &sam, "198.51.100.2", "Fresh", 4).await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    assert_eq!(created.as_object().unwrap().len(), 8, "{created}");
}

#[tokio::test]
async fn the_public_list_needs_no_credential_and_matches_the_detailed_listing() {
    let (bridge, fake) = start().await;
    let (kate, sam, lee) = (
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
    );
    let late = open_room(&bridge, &kate, "198.51.100.1", 8).await;
    let quiet = open_room(&bridge, &sam, "198.51.100.2", 4).await;
    report(&fake, &late, 2, 1, &[]);
    report_details(&fake, &late, full_details());
    ember_bridge::poll_rooms(bridge.state()).await;

    let public = |bridge: &Bridge| {
        let request = bridge.client.get(bridge.url("/v1/rooms/public")).send();
        async move { common::read(request.await.unwrap()).await }
    };
    // A room with no member yet is not listed, here as in Ember.
    let (status, body) = public(&bridge).await;
    assert_eq!(status, StatusCode::OK, "{body}");
    assert_eq!(body["bridge_id"], bridge.bridge_id.as_str());
    assert!(body["listed_at"].as_u64().is_some(), "{body}");
    let detailed = raw_listing(&bridge, &lee, "?detail=1").await;
    assert_eq!(body["rooms"], detailed["rooms"]);
    assert_eq!(body["rooms"].as_array().unwrap().len(), 1, "{body}");
    assert_eq!(body["rooms"][0]["host_name"], "Kate");

    report(&fake, &quiet, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let (_, body) = public(&bridge).await;
    assert_eq!(
        body["rooms"],
        raw_listing(&bridge, &lee, "?detail=1").await["rooms"]
    );
    assert_eq!(body["rooms"].as_array().unwrap().len(), 2, "{body}");
    // Nothing secret rides along.
    assert!(!body.to_string().contains("invitation"), "{body}");

    // A bridge without public rooms has no public list either.
    let off = Bridge::start().await;
    let (status, _) = public(&off).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
}
