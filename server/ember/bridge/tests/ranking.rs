//! The ranking routes: a player's own standing, their choice to take part
//! and the board of classes. Choices belong to the Discord account; a player
//! who does not take part has no class anywhere and their games count for
//! neither fighter.
mod common;

use common::{
    Bridge, Player,
    public_rooms::{open_room, player, send},
    sites::connect_discord,
    supervisor::{BUILD, ENDPOINT, Supervisor, enable, fake_supervisor, report, secrets},
};
use reqwest::{Method, StatusCode};
use serde_json::{Value as Json_, json};

const ME: &str = "/v1/ranking/me";
const BOARD: &str = "/v1/ranking/board";
const DAY: i64 = 24 * 60 * 60;

async fn start(show: bool) -> (Bridge, Supervisor) {
    let (fake, url) = fake_supervisor().await;
    let bridge = Bridge::start_with(
        |config| {
            enable(config, url);
            config.rooms.as_mut().unwrap().skill_badges = show;
        },
        secrets(),
    )
    .await;
    (bridge, fake)
}

async fn call(
    bridge: &Bridge,
    player: &Player,
    method: Method,
    path: &str,
    body: Option<Json_>,
) -> (StatusCode, Json_) {
    send(bridge, method, player.token(), "198.51.100.7", path, body).await
}

async fn standing(bridge: &Bridge, player: &Player) -> Json_ {
    let (status, standing) = call(bridge, player, Method::GET, ME, None).await;
    assert_eq!(status, StatusCode::OK, "{standing}");
    standing
}

async fn choose(bridge: &Bridge, player: &Player, choice: Json_) -> (StatusCode, Json_) {
    call(bridge, player, Method::PUT, ME, Some(choice)).await
}

async fn board(bridge: &Bridge, player: &Player) -> Json_ {
    let (status, board) = call(bridge, player, Method::GET, BOARD, None).await;
    assert_eq!(status, StatusCode::OK, "{board}");
    board["players"].clone()
}

fn listed(name: &str, country: &str) -> Json_ {
    json!({ "ranked": true, "listing": { "name": name, "country": country } })
}

/// Gives Discord account `account` a rating in the middle of `band`, shown
/// last, settled (or not) and last played `days_ago`.
async fn rate(bridge: &Bridge, account: &str, deviation: f64, band: u8, days_ago: i64) {
    let played_at = bridge.state().now() as i64 - days_ago * DAY;
    let rating = 1050.0 + 100.0 * f64::from(band);
    let account = account.to_owned();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO skill_ratings
                     (discord_user_id, rating, deviation, volatility, band, played_at)
                 VALUES (?1, ?2, ?3, 0.06, ?4, ?5)",
                rusqlite::params![account, rating, deviation, band, played_at],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

/// Every stored rating, by Discord account.
async fn ratings(bridge: &Bridge) -> Vec<(String, f64)> {
    bridge
        .state()
        .db
        .read(|tx| {
            let mut statement = tx.prepare(
                "SELECT discord_user_id, rating FROM skill_ratings ORDER BY discord_user_id",
            )?;
            let rows = statement
                .query_map([], |row| Ok((row.get(0)?, row.get(1)?)))?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            Ok(rows)
        })
        .await
        .unwrap()
}

/// The badge in `player`'s ticket for `room_id`.
async fn ticket_badge(bridge: &Bridge, player: &Player, room_id: &str) -> Json_ {
    let body = json!({ "endpoint_id": ENDPOINT, "build_id": BUILD, "skill_badge": true });
    let path = format!("/v1/rooms/{room_id}/tickets");
    let (status, admission) = call(bridge, player, Method::POST, &path, Some(body)).await;
    assert_eq!(status, StatusCode::CREATED, "{admission}");
    admission["ticket"]["ticket"]
        .get("badge")
        .cloned()
        .unwrap_or(Json_::Null)
}

fn report_game(fake: &Supervisor, room_id: &str, generation: u64, winner: &Player, loser: &Player) {
    let mut fake = fake.lock().unwrap();
    let room = fake
        .rooms
        .iter_mut()
        .find(|room| room["room_id"] == room_id)
        .expect("the supervisor hosts the room");
    room["games"] = json!([{ "table": 0, "generation": generation,
        "winner": winner.id().as_str(), "loser": loser.id().as_str() }]);
}

#[tokio::test]
async fn a_standing_says_where_the_player_is() {
    let (bridge, _fake) = start(true).await;
    let (none, placing, settled, faint) = (
        player(&bridge, 1).await,
        player(&bridge, 2).await,
        player(&bridge, 3).await,
        player(&bridge, 4).await,
    );
    assert_eq!(
        standing(&bridge, &none).await,
        json!({ "ranking": "no_account" })
    );

    connect_discord(&bridge, "200", &placing).await;
    assert_eq!(
        standing(&bridge, &placing).await,
        json!({ "ranking": "ranked", "badge": { "band": 0, "settled": false } })
    );

    connect_discord(&bridge, "300", &settled).await;
    rate(&bridge, "300", 60.0, 5, 0).await;
    let (status, chosen) = choose(&bridge, &settled, listed("Player 3", "GB")).await;
    assert_eq!(status, StatusCode::OK, "{chosen}");
    assert_eq!(
        chosen,
        json!({ "ranking": "ranked", "badge": { "band": 5, "settled": true },
            "listing": { "name": "Player 3", "country": "GB" } })
    );

    // Over a year without a game: the class shows faint.
    connect_discord(&bridge, "400", &faint).await;
    rate(&bridge, "400", 60.0, 7, 600).await;
    assert_eq!(
        standing(&bridge, &faint).await["badge"],
        json!({ "band": 7, "settled": false })
    );
}

#[tokio::test]
async fn opting_out_hides_the_class_stops_rating_and_keeps_the_rating() {
    let (bridge, fake) = start(true).await;
    let (mut first, second) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect_discord(&bridge, "100", &first).await;
    connect_discord(&bridge, "200", &second).await;
    let room_id = open_room(&bridge, &first, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 1, &[]);
    report_game(&fake, &room_id, 1, &first, &second);
    ember_bridge::poll_rooms(bridge.state()).await;
    let rated = ratings(&bridge).await;
    assert_eq!(rated.len(), 2);

    let (status, out) = choose(&bridge, &first, json!({ "ranked": false })).await;
    assert_eq!(status, StatusCode::OK, "{out}");
    assert_eq!(out, json!({ "ranking": "opted_out", "change_in": DAY }));
    // No class on a ticket, and a game with them counts for neither fighter.
    assert_eq!(ticket_badge(&bridge, &first, &room_id).await, Json_::Null);
    report_game(&fake, &room_id, 2, &second, &first);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(ratings(&bridge).await, rated);

    // Coming back at once is refused; a day later the same rating resumes.
    let (status, refused) = choose(&bridge, &first, listed("Player 1", "FR")).await;
    assert_eq!(status, StatusCode::CONFLICT, "{refused}");
    assert_eq!(refused["error"]["details"]["reason"], "choice_too_soon");
    bridge.clock.advance(DAY);
    bridge.open_session(&mut first).await;
    let (status, back) = choose(&bridge, &first, listed("Player 1", "FR")).await;
    assert_eq!(status, StatusCode::OK, "{back}");
    assert_eq!(back["ranking"], "ranked");
    assert_eq!(ratings(&bridge).await, rated);
    report_game(&fake, &room_id, 3, &first, &second);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert!(ratings(&bridge).await[0].1 > rated[0].1);
}

#[tokio::test]
async fn the_listing_changes_freely_and_needs_a_discord_account() {
    let (bridge, _fake) = start(true).await;
    let (linked, unlinked) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect_discord(&bridge, "100", &linked).await;
    for name in ["Player 1", "Player 1 again"] {
        let (status, chosen) = choose(&bridge, &linked, listed(name, "JP")).await;
        assert_eq!(status, StatusCode::OK, "{chosen}");
        assert_eq!(chosen["listing"]["name"], name);
        assert!(chosen.get("change_in").is_none(), "{chosen}");
    }
    let (status, refused) = choose(&bridge, &unlinked, listed("Player 2", "JP")).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{refused}");
    assert_eq!(refused["error"]["details"]["reason"], "no_discord_account");
    // A listing is a player name and a two-letter country, only while taking part.
    for invalid in [
        listed(" Player 1", "JP"),
        listed("Player 1", "jp"),
        json!({ "ranked": false, "listing": { "name": "Player 1" } }),
        json!({ "ranked": true, "rating": 1500 }),
    ] {
        let (status, refused) = choose(&bridge, &linked, invalid.clone()).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{invalid} {refused}");
    }
}

#[tokio::test]
async fn the_board_lists_settled_classes_of_listed_players_highest_first() {
    let (bridge, _fake) = start(true).await;
    let mut players = Vec::new();
    for byte in 1..=7u8 {
        let player = player(&bridge, byte).await;
        connect_discord(&bridge, &byte.to_string(), &player).await;
        players.push(player);
    }
    // 1 and 2 share a class; 2 played more recently. 3 is higher.
    rate(&bridge, "1", 60.0, 5, 3).await;
    rate(&bridge, "2", 60.0, 5, 1).await;
    rate(&bridge, "3", 60.0, 8, 2).await;
    // Still placing, faint, unlisted, and opted out: none are on it.
    rate(&bridge, "4", 300.0, 0, 0).await;
    rate(&bridge, "5", 60.0, 9, 600).await;
    rate(&bridge, "6", 60.0, 9, 0).await;
    rate(&bridge, "7", 60.0, 9, 0).await;
    for (index, player) in players.iter().enumerate() {
        if index == 5 {
            continue;
        }
        let name = format!("Player {}", index + 1);
        let (status, chosen) = choose(&bridge, player, listed(&name, "US")).await;
        assert_eq!(status, StatusCode::OK, "{chosen}");
    }
    let (status, out) = choose(&bridge, &players[6], json!({ "ranked": false })).await;
    assert_eq!(status, StatusCode::OK, "{out}");

    let entry = |name: &str, band: u8, own: bool| {
        let mut entry = json!({ "listing": { "name": name, "country": "US" }, "band": band });
        if own {
            entry["own"] = json!(true);
        }
        entry
    };
    assert_eq!(
        board(&bridge, &players[0]).await,
        json!([
            entry("Player 3", 8, false),
            entry("Player 2", 5, false),
            entry("Player 1", 5, true)
        ])
    );
}

#[tokio::test]
async fn while_classes_are_hidden_nothing_shows_but_the_choice_holds() {
    let (bridge, _fake) = start(false).await;
    let first = player(&bridge, 1).await;
    connect_discord(&bridge, "100", &first).await;
    rate(&bridge, "100", 60.0, 5, 0).await;
    let (status, chosen) = choose(&bridge, &first, listed("Player 1", "DE")).await;
    assert_eq!(status, StatusCode::OK, "{chosen}");
    assert_eq!(
        chosen,
        json!({ "ranking": "ranked", "listing": { "name": "Player 1", "country": "DE" } })
    );
    assert_eq!(board(&bridge, &first).await, json!([]));
}

#[tokio::test]
async fn a_bridge_without_public_rooms_has_no_ranking() {
    let bridge = Bridge::start().await;
    let first = player(&bridge, 1).await;
    for (method, path) in [(Method::GET, ME), (Method::PUT, ME), (Method::GET, BOARD)] {
        let body = (method == Method::PUT).then(|| json!({ "ranked": false }));
        let (status, _) = call(&bridge, &first, method, path, body).await;
        assert_eq!(status, StatusCode::NOT_FOUND, "{path}");
    }
    let (_, capabilities) = bridge.get(first.token(), "/v1/capabilities").await;
    assert!(
        !capabilities["features"]
            .as_array()
            .unwrap()
            .contains(&json!("ranking")),
        "{capabilities}"
    );
}

#[tokio::test]
async fn a_bridge_with_public_rooms_offers_ranking() {
    let (bridge, _fake) = start(true).await;
    let first = player(&bridge, 1).await;
    let (_, capabilities) = bridge.get(first.token(), "/v1/capabilities").await;
    assert!(
        capabilities["features"]
            .as_array()
            .unwrap()
            .contains(&json!("ranking")),
        "{capabilities}"
    );
}

#[tokio::test]
async fn a_developer_shows_the_mark_for_builds_that_ask_and_is_not_on_the_board() {
    let (fake, url) = fake_supervisor().await;
    let bridge = Bridge::start_with(
        |config| {
            enable(config, url);
            let rooms = config.rooms.as_mut().unwrap();
            rooms.skill_badges = true;
            rooms.developers = vec!["200".to_owned()];
        },
        secrets(),
    )
    .await;
    let (first, developer) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect_discord(&bridge, "100", &first).await;
    connect_discord(&bridge, "200", &developer).await;
    rate(&bridge, "100", 60.0, 5, 0).await;
    rate(&bridge, "200", 60.0, 8, 0).await;
    for player in [&first, &developer] {
        let (status, chosen) = choose(&bridge, player, listed("Player", "US")).await;
        assert_eq!(status, StatusCode::OK, "{chosen}");
    }
    let room_id = open_room(&bridge, &first, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 1, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;

    // A build that shows the mark gets it in place of the class; one that
    // does not gets the class, so its helper never meets the new field.
    let ticket = |ask: bool| {
        let mut body = json!({ "endpoint_id": ENDPOINT, "build_id": BUILD, "skill_badge": true });
        if ask {
            body["developer_badge"] = json!(true);
        }
        body
    };
    let path = format!("/v1/rooms/{room_id}/tickets");
    for (ask, expected) in [
        (
            true,
            json!({ "band": 0, "settled": false, "developer": true }),
        ),
        (false, json!({ "band": 8, "settled": true })),
    ] {
        let (status, admission) =
            call(&bridge, &developer, Method::POST, &path, Some(ticket(ask))).await;
        assert_eq!(status, StatusCode::CREATED, "{admission}");
        assert_eq!(admission["ticket"]["ticket"]["badge"], expected);
    }
    // Anyone else is shown their class either way.
    let (_, admission) = call(&bridge, &first, Method::POST, &path, Some(ticket(true))).await;
    assert_eq!(
        admission["ticket"]["ticket"]["badge"],
        json!({ "band": 5, "settled": true })
    );

    assert_eq!(
        standing(&bridge, &developer).await["badge"],
        json!({ "band": 0, "settled": false, "developer": true })
    );
    let bands: Vec<Json_> = board(&bridge, &first)
        .await
        .as_array()
        .unwrap()
        .iter()
        .map(|entry| entry["band"].clone())
        .collect();
    assert_eq!(bands, vec![json!(5)]);
}
