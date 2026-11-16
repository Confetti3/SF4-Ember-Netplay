//! A public room's area: where its creator plays from, which they give when
//! they create it and every listing shows as the room's place, beside the
//! relay code that stays in `region` for routing and for 1.5.7 builds.
mod common;

use common::{
    code,
    public_rooms::{create_room_with, listing, open_room, player, start, ticket},
    supervisor::{BUILD, report},
};
use ember_protocol::rooms::{AREA_FEATURE, RoomSummary};
use reqwest::StatusCode;
use serde_json::json;

#[tokio::test]
async fn a_room_is_listed_with_its_creators_area_beside_its_relay() {
    let (bridge, fake) = start().await;
    let (kate, sam, lee) = (
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
    );
    let (_, capabilities) = bridge.get(kate.token(), "/v1/capabilities").await;
    assert!(
        capabilities["features"]
            .as_array()
            .unwrap()
            .contains(&json!(AREA_FEATURE)),
        "{capabilities}"
    );
    let body = json!({ "name": "Montevideo nights", "capacity": 4, "build_id": BUILD,
        "region": "use1", "area": "sam" });
    let (status, room) = create_room_with(&bridge, &kate, "198.51.100.1", body).await;
    assert_eq!(status, StatusCode::CREATED, "{room}");
    assert_eq!(
        (&room["area"], &room["region"]),
        (&json!("sam"), &json!("use1"))
    );
    let placed = room["room_id"].as_str().unwrap().to_owned();
    // A creator who hid their area, or a build from before areas, gives none.
    let hidden = open_room(&bridge, &sam, "198.51.100.2", 4).await;
    report(&fake, &placed, 1, 0, &[]);
    report(&fake, &hidden, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;

    let area_of = |rooms: &[RoomSummary], room_id: &str| {
        let room = rooms.iter().find(|room| room.room_id == room_id).unwrap();
        (room.area.clone(), room.region.clone())
    };
    for query in ["", "?detail=1"] {
        let rooms = listing(&bridge, &lee, query).await;
        assert_eq!(
            area_of(&rooms, &placed),
            (Some("sam".into()), "use1".into()),
            "{query}"
        );
        assert_eq!(area_of(&rooms, &hidden), (None, "use1".into()), "{query}");
    }
    // The web rooms page reads the same, and a room without one has no key.
    let request = bridge.client.get(bridge.url("/v1/rooms/public")).send();
    let (_, public) = common::read(request.await.unwrap()).await;
    let rooms = public["rooms"].as_array().unwrap();
    let raw = |room_id: &str| {
        rooms
            .iter()
            .find(|room| room["room_id"] == room_id)
            .unwrap()
            .clone()
    };
    assert_eq!(raw(&placed)["area"], "sam");
    assert!(raw(&hidden).get("area").is_none(), "{}", raw(&hidden));
    // A ticket's room says it too.
    let (status, admission) = ticket(&bridge, &lee, "198.51.100.3", &placed, BUILD).await;
    assert_eq!(status, StatusCode::CREATED, "{admission}");
    assert_eq!(admission["room"]["area"], "sam");
}

#[tokio::test]
async fn an_area_that_is_not_one_of_the_areas_is_refused() {
    let (bridge, fake) = start().await;
    let kate = player(&bridge, 1).await;
    for area in [
        json!(""),
        json!("use1"),
        json!("SAM"),
        json!("na-w"),
        json!(3),
    ] {
        let body = json!({ "name": "Room", "capacity": 4, "build_id": BUILD, "area": area });
        let (status, body) = create_room_with(&bridge, &kate, "198.51.100.1", body).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{area}");
        assert_eq!(code(&body), "invalid_request", "{area}");
    }
    assert!(fake.lock().unwrap().created.is_empty());
    // None of that used up the creator's one room.
    open_room(&bridge, &kate, "198.51.100.1", 4).await;
}
