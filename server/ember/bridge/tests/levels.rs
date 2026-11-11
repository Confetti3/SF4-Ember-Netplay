//! Fighter points: the games a public room host reports earn each player's
//! fighter points beside the rating, every game of a battle counts, the
//! repeat cap holds for every pair, and a room member's card token opens
//! another player's card for seven days. Migration 057 keeps every rated
//! game, sitting and rating it finds.
mod common;

use std::path::Path;

use common::{
    Bridge, Player,
    public_rooms::{open_room, player, send, start},
    sites::connect_discord,
    supervisor::{BUILD, ENDPOINT, Supervisor, report},
};
use ember_bridge::{Db, testing::TempDir};
use ember_protocol::{
    encoding::b64u,
    levels::{CARD_PREFIX, CARD_VALID_SECS, LOOKUP_PATH, POINTS_PATH, is_card_token},
};
use reqwest::{Method, StatusCode};
use serde_json::{Value as Json_, json};

const FRIEND: &str = "fr_11111111-1111-4111-8111-111111111111";

/// A bridge with public rooms, two players with Discord accounts 100 and 200,
/// and an open room both can join.
async fn two_players() -> (Bridge, Supervisor, String, Player, Player) {
    let (bridge, fake) = start().await;
    let (first, second) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect_discord(&bridge, "100", &first).await;
    connect_discord(&bridge, "200", &second).await;
    let room_id = open_room(&bridge, &first, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 1, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    (bridge, fake, room_id, first, second)
}

/// A won game as a room host reports it: `game` and `fighters` only when a
/// 1.6.0 host would write them.
fn game(
    generation: u64,
    index: u32,
    winner: &Player,
    loser: &Player,
    fighters: Option<[u8; 2]>,
) -> Json_ {
    let mut game = json!({ "table": 0, "generation": generation,
        "winner": winner.id().as_str(), "loser": loser.id().as_str() });
    if index > 0 {
        game["game"] = json!(index);
    }
    if let Some(fighters) = fighters {
        game["fighters"] = json!(fighters);
    }
    game
}

/// Reports `games` as the room's recent games and polls once.
async fn play(bridge: &Bridge, fake: &Supervisor, room_id: &str, games: Vec<Json_>) {
    {
        let mut fake = fake.lock().unwrap();
        let room = fake
            .rooms
            .iter_mut()
            .find(|room| room["room_id"] == room_id)
            .expect("the supervisor hosts the room");
        room["games"] = Json_::Array(games);
    }
    ember_bridge::poll_rooms(bridge.state()).await;
}

async fn sql(bridge: &Bridge, statement: &'static str) {
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute_batch(statement)?;
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

/// Discord account `account`'s stored (fighter, games, wins).
async fn fighters(bridge: &Bridge, account: &'static str) -> Vec<(u8, u64, u64)> {
    bridge
        .state()
        .db
        .read(move |tx| {
            let mut statement = tx.prepare(
                "SELECT fighter, games, wins FROM fighter_levels
                 WHERE discord_user_id = ?1 ORDER BY fighter",
            )?;
            let rows = statement
                .query_map([account], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            Ok(rows)
        })
        .await
        .unwrap()
}

/// `GET /v1/points/me` for `player`, with each fighter's `since` checked and
/// left out.
async fn own(bridge: &Bridge, player: &Player) -> Json_ {
    let (status, mut levels) = send(
        bridge,
        Method::GET,
        player.token(),
        "198.51.100.7",
        POINTS_PATH,
        None,
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{levels}");
    let parsed: ember_protocol::levels::OwnLevels = serde_json::from_value(levels.clone()).unwrap();
    parsed.check().unwrap();
    let fighters = levels
        .get_mut("fighters")
        .and_then(Json_::as_array_mut)
        .into_iter()
        .flatten();
    for fighter in fighters {
        let since = fighter.as_object_mut().unwrap().remove("since").unwrap();
        assert!(since.as_u64().unwrap() > 0, "{since}");
    }
    levels
}

async fn lookup(bridge: &Bridge, player: &Player, cards: Json_) -> (StatusCode, Json_) {
    send(
        bridge,
        Method::POST,
        player.token(),
        "198.51.100.7",
        LOOKUP_PATH,
        Some(json!({ "cards": cards })),
    )
    .await
}

/// The cards `player` sees for `selectors`.
async fn cards(bridge: &Bridge, player: &Player, selectors: Json_) -> Json_ {
    let (status, answer) = lookup(bridge, player, selectors).await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    let parsed: ember_protocol::levels::CardAnswers =
        serde_json::from_value(answer.clone()).unwrap();
    parsed.check().unwrap();
    answer["cards"].clone()
}

/// `player`'s ticket for `room_id`, asking for a card token or not.
async fn ticket(bridge: &Bridge, player: &Player, room_id: &str, card: bool) -> Json_ {
    let mut body = json!({ "endpoint_id": ENDPOINT, "build_id": BUILD });
    if card {
        body["card"] = json!(true);
    }
    let (status, admission) = send(
        bridge,
        Method::POST,
        player.token(),
        "198.51.100.7",
        &format!("/v1/rooms/{room_id}/tickets"),
        Some(body),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED, "{admission}");
    admission["ticket"]["ticket"].clone()
}

/// The card token in `player`'s ticket.
async fn token(bridge: &Bridge, player: &Player, room_id: &str) -> String {
    let ticket = ticket(bridge, player, room_id, true).await;
    ticket["card"].as_str().expect("a card token").to_owned()
}

/// A token sealed by `bridge` with these bytes, as `cards::issue` lays them
/// out: the purpose, the Discord user ID, the issue time.
fn sealed(bridge: &Bridge, purpose: u8, account: u64, issued_at: u64) -> String {
    let mut plain = vec![purpose];
    plain.extend(account.to_le_bytes());
    plain.extend(issued_at.to_le_bytes());
    format!("{CARD_PREFIX}{}", b64u(&bridge.state().keys.seal(&plain)))
}

#[tokio::test]
async fn each_players_fighter_earns_points_from_a_game() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    let unlinked = player(&bridge, 3).await;
    assert_eq!(
        own(&bridge, &first).await,
        json!({ "points": "counted", "shown": true, "fighters": [] })
    );
    assert_eq!(
        own(&bridge, &unlinked).await,
        json!({ "points": "no_account" })
    );

    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(1, 0, &first, &second, Some([3, 7]))],
    )
    .await;
    assert_eq!(
        own(&bridge, &first).await,
        json!({ "points": "counted", "shown": true, "fighters": [
            { "fighter": 3, "points": 2, "games": 1, "wins": 1, "tier": 0, "floor": 0, "next": 200 }] })
    );
    assert_eq!(
        own(&bridge, &second).await,
        json!({ "points": "counted", "shown": true, "fighters": [
            { "fighter": 7, "points": 1, "games": 1, "wins": 0, "tier": 0, "floor": 0, "next": 200 }] })
    );
    // The poll after reports the same game: it counted once.
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(fighters(&bridge, "100").await, vec![(3, 1, 1)]);

    // A second win on another fighter, then more games on the first: a win is 2 points.
    play(
        &bridge,
        &fake,
        &room_id,
        vec![
            game(2, 0, &first, &second, Some([0, 7])),
            game(3, 0, &first, &second, Some([3, 7])),
            game(4, 0, &first, &second, Some([3, 7])),
        ],
    )
    .await;
    assert_eq!(
        own(&bridge, &first).await["fighters"],
        json!([
            { "fighter": 0, "points": 2, "games": 1, "wins": 1, "tier": 0, "floor": 0, "next": 200 },
            { "fighter": 3, "points": 6, "games": 3, "wins": 3, "tier": 0, "floor": 0, "next": 200 }])
    );
    // A game with a fighter who has no Discord account counts for nobody.
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(5, 0, &first, &unlinked, Some([3, 1]))],
    )
    .await;
    assert_eq!(fighters(&bridge, "100").await, vec![(0, 1, 1), (3, 3, 3)]);
}

#[tokio::test]
async fn every_game_of_a_battle_played_on_in_place_counts() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    // Three games of one battle: the same table and generation. A host lists
    // its recent games on every poll, so each poll sees one more.
    let battle: Vec<Json_> = (0..3)
        .map(|index| game(1, index, &first, &second, Some([3, 7])))
        .collect();
    let mut before = 1500.0;
    for seen in 1..=battle.len() {
        play(&bridge, &fake, &room_id, battle[..seen].to_vec()).await;
        let rating = ratings(&bridge).await[0].1;
        assert!(rating > before, "game {seen}: {rating} after {before}");
        before = rating;
        assert_eq!(
            fighters(&bridge, "100").await,
            vec![(3, seen as u64, seen as u64)]
        );
    }

    // A host from before 1.6.0 says nothing of the game in its battle: a
    // second game of a battle reads as its first, as it always did.
    let old = game(2, 0, &first, &second, None);
    play(&bridge, &fake, &room_id, vec![old.clone()]).await;
    let rated = ratings(&bridge).await;
    play(&bridge, &fake, &room_id, vec![old.clone(), old]).await;
    assert_eq!(ratings(&bridge).await, rated);
}

#[tokio::test]
async fn past_eight_games_in_a_sitting_a_game_earns_no_points() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    let sitting: Vec<Json_> = (1..=9)
        .map(|generation| game(generation, 0, &first, &second, Some([3, 7])))
        .collect();
    play(&bridge, &fake, &room_id, sitting).await;
    assert_eq!(fighters(&bridge, "100").await, vec![(3, 8, 8)]);
    assert_eq!(fighters(&bridge, "200").await, vec![(7, 8, 0)]);
    // The pair's record counts every game.
    let record: (String, String, u64, u64) = bridge
        .state()
        .db
        .read(|tx| {
            Ok(tx.query_row(
                "SELECT first, second, first_wins, second_wins FROM pair_records",
                [],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
            )?)
        })
        .await
        .unwrap();
    assert_eq!(record, ("100".into(), "200".into(), 9, 0));

    // Three hours without a game end the sitting.
    bridge.clock.advance(3 * 60 * 60 + 1);
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(10, 0, &second, &first, Some([7, 3]))],
    )
    .await;
    assert_eq!(fighters(&bridge, "100").await, vec![(3, 9, 8)]);
    assert_eq!(fighters(&bridge, "200").await, vec![(7, 9, 1)]);
}

#[tokio::test]
async fn a_game_without_fighters_is_rated_and_earns_no_points() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(1, 0, &first, &second, None)],
    )
    .await;
    let rated = ratings(&bridge).await;
    assert_eq!(rated.len(), 2);
    assert!(fighters(&bridge, "100").await.is_empty());
    assert_eq!(own(&bridge, &first).await["fighters"], json!([]));

    // Fighters outside the roster, or not two of them: the game does not
    // read, so it is skipped alone.
    play(
        &bridge,
        &fake,
        &room_id,
        vec![
            game(2, 0, &first, &second, Some([44, 3])),
            json!({ "table": 0, "generation": 3, "winner": first.id().as_str(),
            "loser": second.id().as_str(), "fighters": [3] }),
        ],
    )
    .await;
    assert_eq!(ratings(&bridge).await, rated);
    assert!(fighters(&bridge, "100").await.is_empty());
}

#[tokio::test]
async fn a_player_who_does_not_take_part_earns_points_and_shows_none() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    sql(
        &bridge,
        "INSERT INTO ranking_choices (discord_user_id, ranked) VALUES ('200', 0)",
    )
    .await;
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(1, 0, &first, &second, Some([3, 7]))],
    )
    .await;
    // No rating moved; both fighters counted.
    assert!(ratings(&bridge).await.is_empty());
    assert_eq!(
        own(&bridge, &second).await,
        json!({ "points": "counted", "fighters": [
            { "fighter": 7, "points": 1, "games": 1, "wins": 0, "tier": 0, "floor": 0, "next": 200 }] })
    );

    // Others see the points of a player who takes part, and none of one who
    // does not: never wins or games.
    let (first_card, second_card) = (
        token(&bridge, &first, &room_id).await,
        token(&bridge, &second, &room_id).await,
    );
    assert_eq!(
        cards(
            &bridge,
            &second,
            json!([{ "card": first_card }, { "card": second_card }])
        )
        .await,
        json!([
            { "fighters": [{ "fighter": 3, "points": 2, "tier": 0 }], "relation": "none" },
            { "relation": "none" }])
    );
}

#[tokio::test]
async fn a_card_token_rides_in_a_ticket_that_asks_and_opens_for_seven_days() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(1, 0, &first, &second, Some([3, 7]))],
    )
    .await;

    // A build that does not ask gets the ticket it always got.
    assert!(
        ticket(&bridge, &second, &room_id, false)
            .await
            .get("card")
            .is_none()
    );
    let joined = token(&bridge, &second, &room_id).await;
    assert!(is_card_token(&joined), "{joined}");
    // A fresh token each join, naming the same player.
    let rejoined = token(&bridge, &second, &room_id).await;
    assert_ne!(joined, rejoined);
    let card =
        json!({ "fighters": [{ "fighter": 7, "points": 1, "tier": 0 }], "relation": "none" });
    assert_eq!(
        cards(
            &bridge,
            &first,
            json!([{ "card": joined }, { "card": rejoined }, { "friend": FRIEND }])
        )
        .await,
        json!([card, card, null])
    );
    // A joiner without a Discord account gets no token.
    let unlinked = player(&bridge, 3).await;
    report(&fake, &room_id, 3, 1, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert!(
        ticket(&bridge, &unlinked, &room_id, true)
            .await
            .get("card")
            .is_none()
    );

    // Seven days from issue, and not a second more. The clock stands still,
    // so the token issued exactly a week ago is still a week old when read.
    bridge.clock.freeze();
    let now = bridge.state().now();
    let (week_old, older) = (
        sealed(&bridge, 0x01, 200, now - CARD_VALID_SECS),
        sealed(&bridge, 0x01, 200, now - CARD_VALID_SECS - 60),
    );
    // Sealed by this bridge for another purpose, or by another bridge.
    let other_purpose = sealed(&bridge, 0x02, 200, now);
    let other_bridge = start().await.0;
    let foreign = sealed(&other_bridge, 0x01, 200, now);
    assert_eq!(
        cards(&bridge, &first, json!([
            { "card": week_old }, { "card": older }, { "card": other_purpose }, { "card": foreign }
        ]))
        .await,
        json!([card, null, null, null])
    );

    // The account no longer connected: its token names nobody.
    sql(
        &bridge,
        "DELETE FROM discord_accounts WHERE user_id = '200'",
    )
    .await;
    assert_eq!(
        cards(&bridge, &first, json!([{ "card": joined }])).await,
        json!([null])
    );
}

#[tokio::test]
async fn a_look_up_takes_one_to_sixteen_well_formed_selectors() {
    let (bridge, _fake, _room_id, first, _second) = two_players().await;
    let token = sealed(&bridge, 0x01, 200, bridge.state().now());
    assert_eq!(
        cards(&bridge, &first, json!(vec![json!({ "card": token }); 16])).await,
        json!(vec![json!({ "fighters": [], "relation": "none" }); 16])
    );
    for bad in [
        json!([]),
        json!(vec![json!({ "card": token }); 17]),
        json!([{ "card": "c1.not a token" }]),
        json!([{ "card": token, "friend": FRIEND }]),
        json!([{ "level": 3 }]),
    ] {
        let (status, body) = lookup(&bridge, &first, bad.clone()).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{bad}: {body}");
    }
}

/// Whether `bridge` lists `points` among its capabilities.
async fn offers_levels(bridge: &Bridge) -> bool {
    let response = bridge
        .client
        .get(bridge.url("/v1/capabilities"))
        .send()
        .await
        .unwrap();
    let (status, body) = common::read(response).await;
    assert_eq!(status, StatusCode::OK, "{body}");
    body["features"]
        .as_array()
        .unwrap()
        .contains(&json!("points"))
}

#[tokio::test]
async fn levels_are_offered_with_public_rooms_and_answer_404_without() {
    let (rooms, _fake) = start().await;
    assert!(offers_levels(&rooms).await);

    let bridge = Bridge::start().await;
    assert!(!offers_levels(&bridge).await);
    let caller = player(&bridge, 1).await;
    let (status, _) = send(
        &bridge,
        Method::GET,
        caller.token(),
        "198.51.100.7",
        POINTS_PATH,
        None,
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = lookup(&bridge, &caller, json!([{ "friend": FRIEND }])).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
}

/// Applies `migrations/` up to 056 as a bridge from before 057 did, each
/// recorded by its number only.
fn database_at_056(path: &Path) -> rusqlite::Connection {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("migrations");
    let mut names: Vec<String> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|entry| entry.unwrap().file_name().into_string().unwrap())
        .filter(|name| name.as_str() < "057")
        .collect();
    names.sort();
    assert_eq!(names.len(), 56);
    let mut connection = rusqlite::Connection::open(path).unwrap();
    connection
        .execute_batch(
            "CREATE TABLE schema_migrations (version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)",
        )
        .unwrap();
    for name in names {
        let version: i64 = name[..3].parse().unwrap();
        let transaction = connection.transaction().unwrap();
        transaction
            .execute_batch(&std::fs::read_to_string(dir.join(&name)).unwrap())
            .unwrap();
        transaction
            .execute(
                "INSERT INTO schema_migrations (version, applied_at) VALUES (?1, 1000)",
                [version],
            )
            .unwrap();
        transaction.commit().unwrap();
    }
    connection
}

/// What the bridge holds of the rating: games rated, sittings and ratings.
fn rating_state(connection: &rusqlite::Connection) -> (Vec<String>, Vec<String>, Vec<String>) {
    let all = |query: &str| -> Vec<String> {
        connection
            .prepare(query)
            .unwrap()
            .query_map([], |row| row.get(0))
            .unwrap()
            .collect::<rusqlite::Result<_>>()
            .unwrap()
    };
    (
        all("SELECT room_id || ':' || table_id || ':' || generation FROM rated_games ORDER BY 1"),
        all(
            "SELECT first || ':' || second || ':' || games || ':' || played_at FROM rated_pairs ORDER BY 1",
        ),
        all(
            "SELECT discord_user_id || ':' || rating || ':' || deviation || ':' || band FROM skill_ratings ORDER BY 1",
        ),
    )
}

#[tokio::test]
async fn migration_057_keeps_rated_games_sittings_and_ratings_and_an_older_bridge_still_works() {
    let dir = TempDir::new("levels-migration");
    let path = dir.join("bridge.sqlite3");
    let before = {
        let connection = database_at_056(&path);
        connection
            .execute_batch(
                "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at)
                   VALUES ('room', 'Friendly matches', 8, 'build', 'emb_player1', 1000);
                 INSERT INTO rated_games (room_id, table_id, generation)
                   VALUES ('room', 0, 1), ('room', 0, 2), ('room', 7, 9223372036854775807);
                 INSERT INTO rated_pairs (first, second, games, played_at)
                   VALUES ('100', '200', 3, 5000), ('100', '300', 9, 6000);
                 INSERT INTO skill_ratings (discord_user_id, rating, deviation, volatility, band, played_at)
                   VALUES ('100', 1612.5, 88.25, 0.06, 6, 5000), ('200', 1387.5, 91.0, 0.06, 4, 5000);",
            )
            .unwrap();
        rating_state(&connection)
    };
    drop(Db::open(&path).unwrap());

    let connection = rusqlite::Connection::open(&path).unwrap();
    assert_eq!(rating_state(&connection), before);
    // Every game rated before is its battle's first.
    let games: Vec<i64> = connection
        .prepare("SELECT game FROM rated_games")
        .unwrap()
        .query_map([], |row| row.get(0))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    assert_eq!(games, vec![0, 0, 0]);
    for table in ["fighter_levels", "pair_records"] {
        let rows: i64 = connection
            .query_row(&format!("SELECT COUNT(*) FROM {table}"), [], |row| {
                row.get(0)
            })
            .unwrap();
        assert_eq!(rows, 0, "{table}");
    }
    // 057 joins the key: the next game of a battle is a game of its own.
    let inserted = |table: i64, generation: i64, game: i64| {
        connection
            .execute(
                "INSERT OR IGNORE INTO rated_games (room_id, table_id, generation, game)
                 VALUES ('room', ?1, ?2, ?3)",
                [table, generation, game],
            )
            .unwrap()
    };
    assert_eq!(inserted(0, 1, 0), 0);
    assert_eq!(inserted(0, 1, 1), 1);

    // A bridge rolled back to before 057 writes what it always wrote: its key
    // is a battle's first game, and the rated games stay rated.
    let older = |table: i64, generation: i64| {
        connection
            .execute(
                "INSERT OR IGNORE INTO rated_games (room_id, table_id, generation)
                 SELECT room_id, ?2, ?3 FROM rooms WHERE room_id = ?1",
                rusqlite::params!["room", table, generation],
            )
            .unwrap()
    };
    assert_eq!(older(0, 2), 0);
    assert_eq!(older(0, 4), 1);
    connection
        .execute_batch(
            "DELETE FROM rated_pairs WHERE played_at < 5500;
             INSERT INTO rated_pairs (first, second, games, played_at) VALUES ('100', '300', 1, 7000)
               ON CONFLICT (first, second) DO UPDATE SET games = games + 1, played_at = 7000;",
        )
        .unwrap();
    drop(connection);

    // Opened again by this bridge, nothing runs twice.
    drop(Db::open(&path).unwrap());
    let connection = rusqlite::Connection::open(&path).unwrap();
    let (rated, pairs, ratings) = rating_state(&connection);
    assert_eq!(rated.len(), 5);
    assert_eq!(pairs, vec!["100:300:10:7000".to_owned()]);
    assert_eq!(ratings, before.2);
}

/// A bridge can hold a rated game whose room was deleted while foreign keys
/// were off. The rebuilt table checks its key as each row lands, so the copy
/// leaves that game behind rather than stopping the bridge from starting.
#[tokio::test]
async fn migration_057_leaves_behind_a_rated_game_whose_room_is_gone_and_keeps_the_rest() {
    let dir = TempDir::new("levels-migration-orphan");
    let path = dir.join("bridge.sqlite3");
    {
        let connection = database_at_056(&path);
        // Foreign keys are off on this connection, as they were for the bridge
        // that lost the room: the delete does not cascade. SQLite here starts
        // with them on, so they are turned off by name.
        connection
            .execute_batch(
                "PRAGMA foreign_keys = OFF;
                 INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at)
                   VALUES ('room', 'Friendly matches', 8, 'build', 'emb_player1', 1000),
                          ('gone', 'Player 1''s room', 8, 'build', 'emb_player1', 1000);
                 INSERT INTO rated_games (room_id, table_id, generation)
                   VALUES ('room', 0, 1), ('gone', 0, 1), ('room', 7, 2), ('gone', 3, 5);
                 DELETE FROM rooms WHERE room_id = 'gone';",
            )
            .unwrap();
        let orphans: i64 = connection
            .query_row(
                "SELECT COUNT(*) FROM rated_games WHERE room_id = 'gone'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(orphans, 2, "the fixture must hold rows without a room");
    }
    drop(Db::open(&path).unwrap());

    let connection = rusqlite::Connection::open(&path).unwrap();
    let kept: Vec<(String, i64, i64, i64)> = connection
        .prepare("SELECT room_id, table_id, generation, game FROM rated_games ORDER BY table_id")
        .unwrap()
        .query_map([], |row| {
            Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?))
        })
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    assert_eq!(
        kept,
        vec![("room".to_owned(), 0, 1, 0), ("room".to_owned(), 7, 2, 0)]
    );
}
