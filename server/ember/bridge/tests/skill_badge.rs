//! The room skill badge: games a room host reports are rated once, between
//! Discord accounts, and the joiner's badge rides in the room ticket only when
//! the bridge shows badges and the joiner's build asks for it.
mod common;

use common::{
    Bridge, Player,
    public_rooms::{open_room, player, send},
    sites::connect_discord,
    supervisor::{BUILD, ENDPOINT, Supervisor, enable, fake_supervisor, report, secrets},
};
use reqwest::StatusCode;
use serde_json::{Value as Json_, json};

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

/// The badge in `player`'s ticket for `room_id`, asking for one or not.
async fn badge(bridge: &Bridge, player: &Player, room_id: &str, ask: bool) -> Json_ {
    let mut body = json!({ "endpoint_id": ENDPOINT, "build_id": BUILD });
    if ask {
        body["skill_badge"] = json!(true);
    }
    let path = format!("/v1/rooms/{room_id}/tickets");
    let (status, admission) = send(
        bridge,
        reqwest::Method::POST,
        player.token(),
        "198.51.100.7",
        &path,
        Some(body),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED, "{admission}");
    admission["ticket"]["ticket"]
        .get("badge")
        .cloned()
        .unwrap_or(Json_::Null)
}

fn game(generation: u64, winner: &Player, loser: &Player) -> Json_ {
    json!({ "table": 0, "generation": generation,
        "winner": winner.id().as_str(), "loser": loser.id().as_str() })
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

/// Every stored rating, by Discord account.
async fn ratings(bridge: &Bridge) -> Vec<(String, f64, u8)> {
    bridge
        .state()
        .db
        .read(|tx| {
            let mut statement = tx.prepare(
                "SELECT discord_user_id, rating, band FROM skill_ratings ORDER BY discord_user_id",
            )?;
            let rows = statement
                .query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            Ok(rows)
        })
        .await
        .unwrap()
}

#[tokio::test]
async fn reported_games_count_once_and_settle_into_a_badge() {
    let (bridge, fake) = start(true).await;
    let (first, second) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect_discord(&bridge, "100", &first).await;
    connect_discord(&bridge, "200", &second).await;
    let room_id = open_room(&bridge, &first, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 1, &[]);
    ember_bridge::poll_rooms(bridge.state()).await;

    // Linked, no games yet: the blank badge.
    assert_eq!(
        badge(&bridge, &second, &room_id, true).await,
        json!({ "band": 0, "settled": false })
    );

    // A host reports its recent games on every poll; each counts once.
    report_games(&fake, &room_id, vec![game(1, &first, &second)]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let once = ratings(&bridge).await;
    assert_eq!(once.len(), 2);
    assert!(once[0].1 > 1500.0 && once[1].1 < 1500.0, "{once:?}");
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(ratings(&bridge).await, once);

    // An even record over enough games, against the room's other regulars,
    // settles by the start. A host lists its newest 32 games, so each poll
    // sees a window of them.
    let (third, fourth) = (player(&bridge, 3).await, player(&bridge, 4).await);
    connect_discord(&bridge, "300", &third).await;
    connect_discord(&bridge, "400", &fourth).await;
    let others = [&second, &third, &fourth];
    let played: Vec<Json_> = (2..46u64)
        .map(|generation| {
            let other = others[generation as usize % 3];
            if (generation / 3) % 2 == 0 {
                game(generation, other, &first)
            } else {
                game(generation, &first, other)
            }
        })
        .collect();
    for end in [20, played.len()] {
        report_games(
            &fake,
            &room_id,
            played[end.saturating_sub(32)..end].to_vec(),
        );
        ember_bridge::poll_rooms(bridge.state()).await;
    }
    let settled = badge(&bridge, &first, &room_id, true).await;
    assert_eq!(settled["settled"], true, "{settled}");
    assert!(
        (4..=5).contains(&settled["band"].as_u64().unwrap()),
        "{settled}"
    );
    // A build that does not ask gets the ticket it always got.
    assert_eq!(badge(&bridge, &first, &room_id, false).await, Json_::Null);
}

#[tokio::test]
async fn past_eight_games_in_a_sitting_a_pair_counts_for_less() {
    let (bridge, fake) = start(true).await;
    let (first, second) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect_discord(&bridge, "100", &first).await;
    connect_discord(&bridge, "200", &second).await;
    let room_id = open_room(&bridge, &first, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 1, &[]);
    // The first player wins every game; record how far each one moves them.
    let mut moved = Vec::new();
    let mut before = 1500.0;
    for generation in 1..=9 {
        report_games(&fake, &room_id, vec![game(generation, &first, &second)]);
        ember_bridge::poll_rooms(bridge.state()).await;
        let now = ratings(&bridge).await[0].1;
        moved.push(now - before);
        before = now;
    }
    assert!(moved[8] < moved[7] * 0.5, "{moved:?}");
}

#[tokio::test]
async fn a_game_with_an_unlinked_fighter_counts_for_neither() {
    let (bridge, fake) = start(true).await;
    let (first, unlinked) = (player(&bridge, 1).await, player(&bridge, 3).await);
    connect_discord(&bridge, "100", &first).await;
    let room_id = open_room(&bridge, &first, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 1, &[]);
    report_games(
        &fake,
        &room_id,
        vec![game(1, &first, &unlinked), game(2, &unlinked, &first)],
    );
    ember_bridge::poll_rooms(bridge.state()).await;
    assert!(ratings(&bridge).await.is_empty());
    // And without a Discord account there is no badge at all.
    assert_eq!(badge(&bridge, &unlinked, &room_id, true).await, Json_::Null);
}

#[tokio::test]
async fn a_game_follows_the_discord_account_not_the_ember_id() {
    let (bridge, fake) = start(true).await;
    let (first, second) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect_discord(&bridge, "100", &first).await;
    connect_discord(&bridge, "200", &second).await;
    let room_id = open_room(&bridge, &first, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 1, &[]);
    report_games(&fake, &room_id, vec![game(1, &first, &second)]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let first_rating = ratings(&bridge).await[0].clone();
    assert_eq!(first_rating.0, "100");

    // The first player's Discord account moves to a new Ember ID: the rating goes with it.
    let renewed = player(&bridge, 4).await;
    let moved = renewed.id().as_str().to_owned();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE discord_accounts SET ember_id = ?1 WHERE user_id = '100'",
                [&moved],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    report_games(&fake, &room_id, vec![game(2, &renewed, &second)]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let after = ratings(&bridge).await;
    assert_eq!(after.len(), 2);
    assert!(after[0].1 > first_rating.1, "{after:?}");
}

#[tokio::test]
async fn a_game_the_bridge_cannot_read_is_skipped_alone() {
    let (bridge, fake) = start(true).await;
    let (first, second) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect_discord(&bridge, "100", &first).await;
    connect_discord(&bridge, "200", &second).await;
    let room_id = open_room(&bridge, &first, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 1, &[]);
    let mut unreadable = game(1, &first, &second);
    unreadable["winner"] = json!("not an Ember ID");
    report_games(
        &fake,
        &room_id,
        vec![unreadable, json!("not a game"), game(2, &first, &second)],
    );
    ember_bridge::poll_rooms(bridge.state()).await;
    // The readable game counted, and the poll did its other work.
    let rated = ratings(&bridge).await;
    assert_eq!(rated.len(), 2);
    assert!(rated[0].1 > 1500.0, "{rated:?}");
    // Games that are not a list count for nothing and fail nothing.
    fake.lock().unwrap().rooms[0]["games"] = json!({ "table": 0 });
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(ratings(&bridge).await, rated);
}

#[tokio::test]
async fn games_are_rated_while_badges_are_hidden() {
    let (bridge, fake) = start(false).await;
    let (first, second) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect_discord(&bridge, "100", &first).await;
    connect_discord(&bridge, "200", &second).await;
    let room_id = open_room(&bridge, &first, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 1, &[]);
    report_games(&fake, &room_id, vec![game(1, &first, &second)]);
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(ratings(&bridge).await.len(), 2);
    assert_eq!(badge(&bridge, &first, &room_id, true).await, Json_::Null);
}

#[tokio::test]
async fn games_of_a_room_the_bridge_does_not_know_are_skipped() {
    let (bridge, fake) = start(true).await;
    let (first, second) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect_discord(&bridge, "100", &first).await;
    connect_discord(&bridge, "200", &second).await;
    let room_id = open_room(&bridge, &first, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 1, &[]);
    fake.lock().unwrap().rooms.push(json!({
        "room_id": "ffffffffffffffffffffffffffffffff", "members": 2, "tables_playing": 1,
        "invitation": "sf4e3:other", "banned": [], "opened": true,
        "games": [game(1, &first, &second)],
    }));
    ember_bridge::poll_rooms(bridge.state()).await;
    assert!(ratings(&bridge).await.is_empty());
    // The poll still did its other work.
    let members: u32 = {
        let room_id = room_id.clone();
        bridge
            .state()
            .db
            .read(move |tx| {
                Ok(tx.query_row(
                    "SELECT members FROM rooms WHERE room_id = ?1",
                    [&room_id],
                    |row| row.get(0),
                )?)
            })
            .await
            .unwrap()
    };
    assert_eq!(members, 2);
}
