//! Public rooms kept apart by game: an SFV Ember room is made, listed, joined
//! and rated only as an SFV room, and every message about an SF4 Ember room is
//! what it was before games existed.
mod common;

use common::{
    Bridge, Player,
    public_rooms::{create_room_with, player, raw_listing, reason, send},
    sites::connect_discord,
    supervisor::{BUILD, ENDPOINT, Supervisor, enable, fake_supervisor, report, secrets},
};
use ember_protocol::rooms::{RoomAdmission, RoomSummary};
use reqwest::StatusCode;
use serde_json::{Value as Json_, json};

/// An SFV Ember build ID, which holds slashes.
const SFV_BUILD: &str = "sfv-ember/13173604/0.2/8dafe602f7615d90";
/// The same ID as a query value.
const SFV_QUERY: &str = "sfv-ember%2F13173604%2F0.2%2F8dafe602f7615d90";

/// A bridge that shows classes, so a badge left out is the game's doing.
async fn start() -> (Bridge, Supervisor) {
    let (fake, url) = fake_supervisor().await;
    let bridge = Bridge::start_with(
        |config| {
            enable(config, url);
            config.rooms.as_mut().unwrap().skill_badges = true;
        },
        secrets(),
    )
    .await;
    (bridge, fake)
}

/// Opens an SFV room as `creator` and an SF4 room as `other`, both with a
/// member, and answers their ids.
async fn rooms(
    bridge: &Bridge,
    fake: &Supervisor,
    creator: &Player,
    other: &Player,
) -> (String, String) {
    let (status, sfv) = create_room_with(
        bridge,
        creator,
        "198.51.100.1",
        json!({ "name": "SFV room", "capacity": 8, "build_id": SFV_BUILD, "game": "sfv" }),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED, "{sfv}");
    assert_eq!(sfv["game"], "sfv");
    assert_eq!(sfv["build_id"], SFV_BUILD);
    let (status, sf4) = create_room_with(
        bridge,
        other,
        "198.51.100.2",
        json!({ "name": "SF4 room", "capacity": 8, "build_id": BUILD }),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED, "{sf4}");
    assert!(sf4.get("game").is_none(), "{sf4}");
    let ids = (
        sfv["room_id"].as_str().unwrap().to_owned(),
        sf4["room_id"].as_str().unwrap().to_owned(),
    );
    report(fake, &ids.0, 1, 0, &[]);
    report(fake, &ids.1, 1, 0, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    (ids.0, ids.1)
}

fn ids(body: &Json_) -> Vec<String> {
    body["rooms"]
        .as_array()
        .unwrap()
        .iter()
        .map(|room| room["room_id"].as_str().unwrap().to_owned())
        .collect()
}

/// `POST /v1/rooms/{room_id}/tickets` with `body` as it is.
async fn ticket_with(
    bridge: &Bridge,
    player: &Player,
    room_id: &str,
    body: Json_,
) -> (StatusCode, Json_) {
    send(
        bridge,
        reqwest::Method::POST,
        player.token(),
        "198.51.100.7",
        &format!("/v1/rooms/{room_id}/tickets"),
        Some(body),
    )
    .await
}

async fn public_page(bridge: &Bridge, query: &str) -> (StatusCode, Json_) {
    common::read(
        bridge
            .client
            .get(bridge.url(&format!("/v1/rooms/public{query}")))
            .send()
            .await
            .unwrap(),
    )
    .await
}

async fn stored_game(bridge: &Bridge, room_id: String) -> String {
    bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT game FROM rooms WHERE room_id = ?1",
                [room_id],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap()
}

#[tokio::test]
async fn a_create_names_its_game_to_the_row_and_the_supervisor_only_when_it_is_not_sf4() {
    let (bridge, fake) = start().await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let (sfv, sf4) = rooms(&bridge, &fake, &player1, &player2).await;
    let sent = fake.lock().unwrap().created.clone();
    assert_eq!(sent.len(), 2);
    assert_eq!(sent[0]["game"], "sfv");
    assert_eq!(sent[0]["build_id"], SFV_BUILD);
    assert!(sent[1].get("game").is_none(), "{}", sent[1]);
    assert_eq!(stored_game(&bridge, sfv).await, "sfv");
    assert_eq!(stored_game(&bridge, sf4).await, "usf4");

    // An unknown game, or one that is not a string, makes no room.
    let player3 = player(&bridge, 3).await;
    for game in [json!("sf6"), json!(""), json!(1), json!(null)] {
        let (status, body) = create_room_with(
            &bridge,
            &player3,
            "198.51.100.3",
            json!({ "name": "Room", "capacity": 8, "build_id": BUILD, "game": game }),
        )
        .await;
        assert!(status.is_client_error(), "{game}: {status} {body}");
    }
    assert_eq!(fake.lock().unwrap().created.len(), 2);
}

#[tokio::test]
async fn a_listing_shows_one_games_rooms_and_sf4_when_it_names_none() {
    let (bridge, fake) = start().await;
    let (player1, player2, player3) = (
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
    );
    let (sfv, sf4) = rooms(&bridge, &fake, &player1, &player2).await;

    // An SF4 build's listing, as SF4 Ember asks for it, and every listing
    // that names no game: the SF4 room alone.
    for query in [
        format!("?build_id={BUILD}&detail=1"),
        format!("?build_id={BUILD}"),
        String::new(),
        "?detail=1".to_owned(),
        format!("?build_id={SFV_QUERY}&detail=1"),
        format!("?build_id={SFV_BUILD}"),
    ] {
        let body = raw_listing(&bridge, &player3, &query).await;
        let listed = ids(&body);
        assert!(!listed.contains(&sfv), "{query}: {body}");
        assert!(!body.to_string().contains("\"game\""), "{query}: {body}");
        let sf4_listed = !query.contains("sfv");
        assert_eq!(listed.contains(&sf4), sf4_listed, "{query}: {body}");
    }
    // With the game: the SFV room, which says so, and never the SF4 room.
    for query in [
        format!("?build_id={SFV_QUERY}&detail=1&game=sfv"),
        format!("?build_id={SFV_BUILD}&game=sfv"),
        "?game=sfv".to_owned(),
    ] {
        let body = raw_listing(&bridge, &player3, &query).await;
        assert_eq!(ids(&body), vec![sfv.clone()], "{query}: {body}");
        assert_eq!(body["rooms"][0]["game"], "sfv", "{query}");
        assert_eq!(body["rooms"][0]["build_id"], SFV_BUILD, "{query}");
        let room: RoomSummary = serde_json::from_value(body["rooms"][0].clone()).unwrap();
        room.check().unwrap();
    }
    // `game=usf4` is the default said out loud.
    let body = raw_listing(&bridge, &player3, &format!("?build_id={BUILD}&game=usf4")).await;
    assert_eq!(ids(&body), vec![sf4.clone()]);
    // A game the bridge does not know is an invalid request.
    for game in ["sf6", "", "USF4"] {
        let (status, body) = send(
            &bridge,
            reqwest::Method::GET,
            player3.token(),
            "198.51.100.1",
            &format!("/v1/rooms?game={game}"),
            None,
        )
        .await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{game}: {body}");
        assert_eq!(body["error"]["code"], "invalid_request", "{body}");
    }
}

#[tokio::test]
async fn the_public_page_lists_sf4_rooms_unless_it_names_a_game() {
    let (bridge, fake) = start().await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let (sfv, sf4) = rooms(&bridge, &fake, &player1, &player2).await;
    let (status, page) = public_page(&bridge, "").await;
    assert_eq!(status, StatusCode::OK, "{page}");
    assert_eq!(ids(&page), vec![sf4.clone()]);
    assert!(!page.to_string().contains("\"game\""), "{page}");
    let (status, page) = public_page(&bridge, "?game=sfv").await;
    assert_eq!(status, StatusCode::OK, "{page}");
    assert_eq!(ids(&page), vec![sfv]);
    assert_eq!(page["rooms"][0]["game"], "sfv");
    let (_, page) = public_page(&bridge, "?game=usf4").await;
    assert_eq!(ids(&page), vec![sf4]);
    let (status, page) = public_page(&bridge, "?game=sf6").await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{page}");
}

#[tokio::test]
async fn a_room_admits_only_its_own_games_build_and_an_sfv_ticket_has_no_class() {
    let (bridge, fake) = start().await;
    let (player1, player2, player3) = (
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
    );
    // Linked, so each would have a class where classes are shown.
    connect_discord(&bridge, "100", &player1).await;
    connect_discord(&bridge, "200", &player2).await;
    connect_discord(&bridge, "300", &player3).await;
    let (sfv, sf4) = rooms(&bridge, &fake, &player1, &player2).await;

    // SF4 Ember's request names no game: the SFV room refuses it, with the
    // SFV build as with its own.
    for build in [SFV_BUILD, BUILD] {
        let (status, body) = ticket_with(
            &bridge,
            &player3,
            &sfv,
            json!({ "endpoint_id": ENDPOINT, "build_id": build, "skill_badge": true }),
        )
        .await;
        assert!(status.is_client_error(), "{build}: {status} {body}");
        assert_eq!(reason(&body), "unsupported_build", "{build}");
    }
    // An SFV request for the SF4 room is refused the same way.
    let (status, body) = ticket_with(
        &bridge,
        &player3,
        &sf4,
        json!({ "endpoint_id": ENDPOINT, "build_id": BUILD, "game": "sfv" }),
    )
    .await;
    assert!(status.is_client_error(), "{status} {body}");
    assert_eq!(reason(&body), "unsupported_build");
    // An unknown game is not a request.
    let (status, body) = ticket_with(
        &bridge,
        &player3,
        &sfv,
        json!({ "endpoint_id": ENDPOINT, "build_id": SFV_BUILD, "game": "sf6" }),
    )
    .await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{body}");

    // SFV Ember's own request is admitted, and its ticket carries no class.
    let (status, admitted) = ticket_with(
        &bridge,
        &player3,
        &sfv,
        json!({ "endpoint_id": ENDPOINT, "build_id": SFV_BUILD, "skill_badge": true,
            "game": "sfv" }),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED, "{admitted}");
    assert!(
        admitted["ticket"]["ticket"].get("badge").is_none(),
        "{admitted}"
    );
    assert_eq!(admitted["room"]["game"], "sfv");
    let parsed: RoomAdmission = serde_json::from_value(admitted).unwrap();
    parsed.check().unwrap();
    assert_eq!(parsed.room.game, "sfv");

    // The SF4 room's ticket for the same player carries the class, and its
    // admission says nothing of a game.
    let (status, admitted) = ticket_with(
        &bridge,
        &player3,
        &sf4,
        json!({ "endpoint_id": ENDPOINT, "build_id": BUILD, "skill_badge": true }),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED, "{admitted}");
    assert!(
        admitted["ticket"]["ticket"].get("badge").is_some(),
        "{admitted}"
    );
    assert!(admitted["room"].get("game").is_none(), "{admitted}");
}

/// What the stand-in reports as `room_id`'s recent games.
fn report_games(fake: &Supervisor, room_id: &str, games: Vec<Json_>) {
    let mut fake = fake.lock().unwrap();
    let room = fake
        .rooms
        .iter_mut()
        .find(|room| room["room_id"] == room_id)
        .expect("the supervisor hosts the room");
    room["games"] = Json_::Array(games);
}

fn game(generation: u64, winner: &Player, loser: &Player) -> Json_ {
    json!({ "table": 0, "generation": generation,
        "winner": winner.id().as_str(), "loser": loser.id().as_str() })
}

/// The rated players and the rated games.
async fn rated(bridge: &Bridge) -> (i64, i64) {
    bridge
        .state()
        .db
        .read(|tx| {
            Ok((
                tx.query_row("SELECT COUNT(*) FROM skill_ratings", [], |row| row.get(0))?,
                tx.query_row("SELECT COUNT(*) FROM rated_games", [], |row| row.get(0))?,
            ))
        })
        .await
        .unwrap()
}

#[tokio::test]
async fn games_in_an_sfv_room_are_not_rated() {
    let (bridge, fake) = start().await;
    let (player1, player2) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect_discord(&bridge, "100", &player1).await;
    connect_discord(&bridge, "200", &player2).await;
    let (sfv, sf4) = rooms(&bridge, &fake, &player1, &player2).await;
    report_games(
        &fake,
        &sfv,
        vec![game(1, &player1, &player2), game(2, &player2, &player1)],
    );
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(rated(&bridge).await, (0, 0));
    // The same two players' game in the SF4 room is rated.
    report_games(&fake, &sf4, vec![game(1, &player1, &player2)]);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(rated(&bridge).await, (2, 1));
}
