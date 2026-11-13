//! The bridge with public rooms, two players with Discord accounts 100 and 200,
//! and the helpers the points, award counter and most played opponent tests
//! share (`levels.rs`, `award_counters.rs`, `rival_card.rs`).
use super::{
    Bridge, Player,
    public_rooms::{open_room, player, send, start, start_showing_classes},
    sites::connect_discord,
    supervisor::{BUILD, ENDPOINT, Supervisor, report},
};
use rusqlite::OptionalExtension;
use ember_protocol::{
    encoding::b64u,
    levels::{CARD_PREFIX, CARD_VALID_SECS, LOOKUP_PATH, POINTS_PATH, is_card_token},
};
use reqwest::{Method, StatusCode};
use serde_json::{Value as Json_, json};

pub const FRIEND: &str = "fr_11111111-1111-4111-8111-111111111111";

/// A bridge with public rooms, two players with Discord accounts 100 and 200,
/// and an open room both can join.
pub async fn two_players() -> (Bridge, Supervisor, String, Player, Player) {
    two_players_on(start().await).await
}

/// `two_players` on a bridge that shows classes in tickets, as the ones that remember a class need.
pub async fn two_players_showing_classes() -> (Bridge, Supervisor, String, Player, Player) {
    two_players_on(start_showing_classes().await).await
}

async fn two_players_on((bridge, fake): (Bridge, Supervisor)) -> (Bridge, Supervisor, String, Player, Player) {
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
pub fn game(
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
pub async fn play(bridge: &Bridge, fake: &Supervisor, room_id: &str, games: Vec<Json_>) {
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

pub async fn sql(bridge: &Bridge, statement: &'static str) {
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
pub async fn ratings(bridge: &Bridge) -> Vec<(String, f64)> {
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
pub async fn fighters(bridge: &Bridge, account: &'static str) -> Vec<(u8, u64, u64)> {
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

/// Discord account `account`'s stored (games, wins) of account points, none
/// before its first counted game.
pub async fn account_points(bridge: &Bridge, account: &'static str) -> Option<(u64, u64)> {
    bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx
                .query_row(
                    "SELECT games, wins FROM account_points WHERE discord_user_id = ?1",
                    [account],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?)
        })
        .await
        .unwrap()
}

/// `GET /v1/points/me` for `player`, with the account's and each fighter's
/// `since` checked and left out, and the account's award counters left out
/// (`awards`).
pub async fn own(bridge: &Bridge, player: &Player) -> Json_ {
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
    if let Some(account) = levels.get_mut("account") {
        let games = account["games"].as_u64().unwrap();
        let since = account.as_object_mut().unwrap().remove("since");
        assert_eq!(since.is_some(), games > 0, "{account}");
        assert!(since.is_none_or(|since| since.as_u64().unwrap() > 0));
        let counters = account.as_object_mut().unwrap();
        counters.remove("best_streak");
        counters.remove("peak_class");
        counters.remove("rival");
        counters.remove("opponents");
    }
    // The most played opponent has its own tests (`rival_of`).
    if let Some(levels) = levels.as_object_mut() {
        levels.remove("rival");
    }
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

pub async fn lookup(bridge: &Bridge, player: &Player, cards: Json_) -> (StatusCode, Json_) {
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
pub async fn cards(bridge: &Bridge, player: &Player, selectors: Json_) -> Json_ {
    let (status, answer) = lookup(bridge, player, selectors).await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    let parsed: ember_protocol::levels::CardAnswers =
        serde_json::from_value(answer.clone()).unwrap();
    parsed.check().unwrap();
    answer["cards"].clone()
}

/// `player`'s ticket for `room_id`, asking for a card token or not.
pub async fn ticket(bridge: &Bridge, player: &Player, room_id: &str, card: bool) -> Json_ {
    // A build that shows classes asks for the class (`skill_badge`); a bridge that does not show them ignores it.
    let mut body = json!({ "endpoint_id": ENDPOINT, "build_id": BUILD, "skill_badge": true });
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
pub async fn token(bridge: &Bridge, player: &Player, room_id: &str) -> String {
    let ticket = ticket(bridge, player, room_id, true).await;
    ticket["card"].as_str().expect("a card token").to_owned()
}

/// A token sealed by `bridge` with these bytes, as `cards::issue` lays them
/// out: the purpose, the Discord user ID, the issue time.
pub fn sealed(bridge: &Bridge, purpose: u8, account: u64, issued_at: u64) -> String {
    let mut plain = vec![purpose];
    plain.extend(account.to_le_bytes());
    plain.extend(issued_at.to_le_bytes());
    format!("{CARD_PREFIX}{}", b64u(&bridge.state().keys.seal(&plain)))
}
