//! Points: the games a public room host reports earn each player's account
//! points and fighter points beside the rating, every game of a battle
//! counts, the repeat cap holds for every pair, and a room member's card token
//! opens another player's card, public whether or not they take part in
//! ranking, for seven days. Its migrations' tests are in `levels_migrations.rs`.
mod common;

use common::{
    Bridge, Player,
    public_rooms::{open_room, player, send, start},
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

/// Discord account `account`'s stored (games, wins) of account points, none
/// before its first counted game.
async fn account_points(bridge: &Bridge, account: &'static str) -> Option<(u64, u64)> {
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
        json!({ "points": "counted", "fighters": [],
            "account": { "points": 0, "games": 0, "wins": 0, "tier": 0, "floor": 0, "next": 500 } })
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
        json!({ "points": "counted",
            "account": { "points": 2, "games": 1, "wins": 1, "tier": 0, "floor": 0, "next": 500 },
            "fighters": [
            { "fighter": 3, "points": 2, "games": 1, "wins": 1, "tier": 0, "floor": 0, "next": 200 }] })
    );
    assert_eq!(
        own(&bridge, &second).await,
        json!({ "points": "counted",
            "account": { "points": 1, "games": 1, "wins": 0, "tier": 0, "floor": 0, "next": 500 },
            "fighters": [
            { "fighter": 7, "points": 1, "games": 1, "wins": 0, "tier": 0, "floor": 0, "next": 200 }] })
    );
    // The poll after reports the same game: it counted once.
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(fighters(&bridge, "100").await, vec![(3, 1, 1)]);
    assert_eq!(account_points(&bridge, "100").await, Some((1, 1)));

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
    // The account counts every game, the fighters each their own.
    assert_eq!(
        own(&bridge, &first).await["account"],
        json!({ "points": 8, "games": 4, "wins": 4, "tier": 0, "floor": 0, "next": 500 })
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
async fn past_eight_games_in_a_sitting_a_game_earns_no_points_of_either_kind() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    let sitting: Vec<Json_> = (1..=9)
        .map(|generation| game(generation, 0, &first, &second, Some([3, 7])))
        .collect();
    play(&bridge, &fake, &room_id, sitting).await;
    assert_eq!(fighters(&bridge, "100").await, vec![(3, 8, 8)]);
    assert_eq!(fighters(&bridge, "200").await, vec![(7, 8, 0)]);
    // The account's points stop at the same game.
    assert_eq!(account_points(&bridge, "100").await, Some((8, 8)));
    assert_eq!(account_points(&bridge, "200").await, Some((8, 0)));
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
    assert_eq!(account_points(&bridge, "100").await, Some((9, 8)));
    assert_eq!(account_points(&bridge, "200").await, Some((9, 1)));
}

#[tokio::test]
async fn a_game_without_fighters_is_rated_and_earns_account_points_only() {
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
    let levels = own(&bridge, &first).await;
    assert_eq!(levels["fighters"], json!([]));
    assert_eq!(
        levels["account"],
        json!({ "points": 2, "games": 1, "wins": 1, "tier": 0, "floor": 0, "next": 500 })
    );
    assert_eq!(account_points(&bridge, "200").await, Some((1, 0)));

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
    assert_eq!(account_points(&bridge, "100").await, Some((1, 1)));
}

/// Production and staging since 2026-11-09 hold the shared library's 057 to
/// 059, which put `game` at the head of the rating tables' keys, and SFV rows
/// beside USF4's. This bridge rates and reads USF4's alone and never touches
/// SFV's: a second game updates the same rows (the conflict targets name the
/// game), and the SFV rows keep what they held.
#[tokio::test]
async fn usf4_games_are_rated_beside_sfv_rows_and_leave_them_alone() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    sql(
        &bridge,
        "INSERT INTO skill_ratings (game, discord_user_id, rating, deviation, volatility, band, played_at)
           VALUES ('sfv', '100', 1900.0, 60.0, 0.06, 8, 5000), ('sfv', '200', 1100.0, 60.0, 0.06, 2, 5000);
         INSERT INTO ranking_choices (game, discord_user_id, ranked, changed_at, name, country)
           VALUES ('sfv', '100', 0, 5000, 'Player 1', NULL);
         INSERT INTO rated_pairs (game, first, second, games, played_at)
           VALUES ('sfv', '100', '200', 4, 5000);",
    )
    .await;
    let sfv = |bridge: &Bridge| {
        let state = bridge.state().clone();
        async move {
            state
                .db
                .read(|tx| {
                    let mut statement = tx.prepare(
                        "SELECT discord_user_id || ':' || rating || ':' || band FROM skill_ratings
                         WHERE game = 'sfv'
                         UNION ALL SELECT first || ':' || second || ':' || games FROM rated_pairs
                         WHERE game = 'sfv'
                         UNION ALL SELECT discord_user_id || ':' || ranked FROM ranking_choices
                         WHERE game = 'sfv' ORDER BY 1",
                    )?;
                    let rows = statement
                        .query_map([], |row| row.get::<_, String>(0))?
                        .collect::<rusqlite::Result<Vec<_>>>()?;
                    Ok(rows)
                })
                .await
                .unwrap()
        }
    };
    let before = sfv(&bridge).await;
    assert_eq!(before.len(), 4);

    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(1, 0, &first, &second, Some([3, 7]))],
    )
    .await;
    play(
        &bridge,
        &fake,
        &room_id,
        vec![
            game(1, 0, &first, &second, Some([3, 7])),
            game(2, 0, &second, &first, Some([7, 3])),
        ],
    )
    .await;
    // Two games: each USF4 rating written once and updated once, the sitting counted twice.
    let usf4: (i64, i64) = {
        let state = bridge.state().clone();
        state
            .db
            .read(|tx| {
                Ok(tx.query_row(
                    "SELECT (SELECT COUNT(*) FROM skill_ratings WHERE game = 'usf4'),
                            (SELECT games FROM rated_pairs WHERE game = 'usf4' AND first = '100' AND second = '200')",
                    [],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )?)
            })
            .await
            .unwrap()
    };
    assert_eq!(usf4, (2, 2));
    assert_eq!(account_points(&bridge, "100").await, Some((2, 1)));
    // A USF4 player's own points and class read USF4's rating, not SFV's.
    assert_eq!(own(&bridge, &first).await["points"], json!("counted"));
    assert_eq!(sfv(&bridge).await, before);
}

#[tokio::test]
async fn a_player_who_does_not_take_part_earns_points_and_shows_them() {
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
    // No rating moved; both accounts and both fighters counted.
    assert!(ratings(&bridge).await.is_empty());
    assert_eq!(
        own(&bridge, &second).await,
        json!({ "points": "counted",
            "account": { "points": 1, "games": 1, "wins": 0, "tier": 0, "floor": 0, "next": 500 },
            "fighters": [
            { "fighter": 7, "points": 1, "games": 1, "wins": 0, "tier": 0, "floor": 0, "next": 200 }] })
    );

    // A card is public: others see the points of a player who does not take
    // part as of one who does, and the games and wins behind them.
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
            { "account": { "points": 2, "tier": 0 },
              "fighters": [{ "fighter": 3, "points": 2, "tier": 0 }], "relation": "none",
              "marks": { "games": 1, "wins": 1 } },
            { "account": { "points": 1, "tier": 0 },
              "fighters": [{ "fighter": 7, "points": 1, "tier": 0 }], "relation": "none",
              "marks": { "games": 1 } }])
    );
}

/// Sets Discord account `account`'s account points directly.
async fn set_account_points(bridge: &Bridge, account: &'static str, games: u64, wins: u64) {
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT OR REPLACE INTO account_points
                   (discord_user_id, games, wins, first_at, played_at)
                 VALUES (?1, ?2, ?3, 1729468800, 1729468800)",
                rusqlite::params![account, games, wins],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

#[tokio::test]
async fn account_points_use_their_own_tier_thresholds() {
    let (bridge, _fake, room_id, first, second) = two_players().await;
    // (games, wins) -> (points, tier, floor, next), at and just under each
    // threshold: 500, 2000, 5000 and 10000.
    for (games, wins, tier, floor, next) in [
        (300, 199, 0, 0, Some(500)),
        (300, 200, 1, 500, Some(2000)),
        (1000, 999, 1, 500, Some(2000)),
        (1000, 1000, 2, 2000, Some(5000)),
        (2500, 2499, 2, 2000, Some(5000)),
        (2500, 2500, 3, 5000, Some(10_000)),
        (5000, 4999, 3, 5000, Some(10_000)),
        (5000, 5000, 4, 10_000, None),
        (9000, 3000, 4, 10_000, None),
    ] {
        set_account_points(&bridge, "100", games, wins).await;
        let points = games + wins;
        let mut want = json!({ "points": points, "games": games, "wins": wins,
            "tier": tier, "floor": floor });
        if let Some(next) = next {
            want["next"] = json!(next);
        }
        assert_eq!(own(&bridge, &first).await["account"], want);
        // The card carries the points and the same tier.
        let card = token(&bridge, &first, &room_id).await;
        assert_eq!(
            cards(&bridge, &second, json!([{ "card": card }])).await[0]["account"],
            json!({ "points": points, "tier": tier })
        );
    }
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
    let card = json!({ "account": { "points": 1, "tier": 0 },
        "fighters": [{ "fighter": 7, "points": 1, "tier": 0 }], "relation": "none",
        "marks": { "games": 1 } });
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
        json!(vec![
            json!({ "account": { "points": 0, "tier": 0 }, "fighters": [], "relation": "none" });
            16
        ])
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

/// SFV Ember's builds call another game's ranking routes only on a bridge
/// that lists `ranking_games`. This line rates USF4 alone, so it never lists
/// it, and an SFV build on this bridge writes no SFV ratings as USF4's
/// (the library's `docs/design/SFV_BRIDGE.md`).
#[tokio::test]
async fn ranking_by_game_is_never_offered() {
    let (bridge, _fake) = start().await;
    let response = bridge
        .client
        .get(bridge.url("/v1/capabilities"))
        .send()
        .await
        .unwrap();
    let (status, body) = common::read(response).await;
    assert_eq!(status, StatusCode::OK, "{body}");
    let features = body["features"].as_array().unwrap();
    assert!(features.contains(&json!("ranking")), "{body}");
    assert!(!features.contains(&json!("ranking_games")), "{body}");
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

/// `GET /v1/points/me`'s raw account: its best run of wins and highest class.
async fn awards(bridge: &Bridge, player: &Player) -> (u64, u64) {
    let (status, levels) = send(
        bridge,
        Method::GET,
        player.token(),
        "198.51.100.7",
        POINTS_PATH,
        None,
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{levels}");
    let account = &levels["account"];
    (
        account["best_streak"].as_u64().unwrap_or(0),
        account["peak_class"].as_u64().unwrap_or(0),
    )
}

#[tokio::test]
async fn the_best_run_of_wins_only_rises_and_a_loss_ends_a_run() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    assert_eq!(awards(&bridge, &first).await, (0, 0));
    // Three wins in a row, then a loss, then two wins: the best is still 3.
    let games = vec![
        game(1, 0, &first, &second, None),
        game(2, 0, &first, &second, None),
        game(3, 0, &first, &second, None),
        game(4, 0, &second, &first, None),
        game(5, 0, &first, &second, None),
        game(6, 0, &first, &second, None),
    ];
    play(&bridge, &fake, &room_id, games).await;
    assert_eq!(awards(&bridge, &first).await.0, 3);
    // The other player's best run is the one win between.
    assert_eq!(awards(&bridge, &second).await.0, 1);
    // The poll after reports the same games: nothing counts twice.
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(awards(&bridge, &first).await.0, 3);

    // The card shows the counts themselves: six games, five wins and the best
    // run of three.
    let card = token(&bridge, &first, &room_id).await;
    let seen = cards(&bridge, &second, json!([{ "card": card }])).await;
    assert_eq!(
        seen[0]["marks"],
        json!({ "games": 6, "wins": 5, "streak": 3 })
    );
}

/// Gives Discord account 100 a settled rating of `rating` as of now, as the
/// ratings of games would leave it.
async fn settle_rating(bridge: &Bridge, rating: f64) {
    let now = bridge.state().now();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO skill_ratings
                   (game, discord_user_id, rating, deviation, volatility, band, played_at)
                 VALUES ('usf4', '100', ?1, 80, 0.06, 0, ?2)
                 ON CONFLICT (game, discord_user_id) DO UPDATE SET rating = ?1, played_at = ?2",
                rusqlite::params![rating, now],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

#[tokio::test]
async fn the_peak_class_never_drops_and_a_card_shows_it_only_while_taking_part() {
    let (bridge, _fake, room_id, first, second) = two_players().await;
    // Each ticket shows the class the rating has reached (B+, then Grand
    // Master, then B+ again): the peak is the highest of them.
    for (rating, peak) in [(1650.0, 6), (2100.0, 10), (1650.0, 10)] {
        settle_rating(&bridge, rating).await;
        ticket(&bridge, &first, &room_id, true).await;
        assert_eq!(awards(&bridge, &first).await.1, peak, "{rating}");
    }
    let card = token(&bridge, &first, &room_id).await;
    let class = |seen: &Json_| seen[0]["marks"]["class"].as_u64();
    assert_eq!(
        class(&cards(&bridge, &second, json!([{ "card": card }])).await),
        Some(10)
    );
    // Not taking part in ranking shows no class on the card, and the player's
    // own answer keeps it.
    sql(
        &bridge,
        "INSERT INTO ranking_choices (discord_user_id, ranked) VALUES ('100', 0)",
    )
    .await;
    assert_eq!(
        class(&cards(&bridge, &second, json!([{ "card": card }])).await),
        None
    );
    assert_eq!(awards(&bridge, &first).await.1, 10);
}

#[tokio::test]
async fn a_class_the_counters_missed_is_still_the_peak() {
    // A class shown by a bridge from before the counters has no row; the
    // class itself is the least the peak can be.
    let (bridge, _fake, _room_id, first, _second) = two_players().await;
    sql(
        &bridge,
        "INSERT INTO skill_ratings (game, discord_user_id, rating, deviation, volatility, band, played_at)
         VALUES ('usf4', '100', 1650, 80, 0.06, 6, 1)",
    )
    .await;
    assert_eq!(awards(&bridge, &first).await.1, 6);
}

#[tokio::test]
async fn the_rival_and_opponent_counts_come_from_the_pair_records() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    let games = (1..=3).map(|n| game(n, 0, &first, &second, None)).collect();
    play(&bridge, &fake, &room_id, games).await;
    let (status, levels) = send(
        &bridge,
        Method::GET,
        first.token(),
        "198.51.100.7",
        POINTS_PATH,
        None,
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{levels}");
    // Three games against one opponent: a rival of 3, one opponent.
    assert_eq!(levels["account"]["rival"], json!(3));
    assert_eq!(levels["account"]["opponents"], json!(1));
    // A card shows the rival and opponent counts as milestones only, and 3
    // games against one player is not one yet.
    let card = token(&bridge, &first, &room_id).await;
    let seen = cards(&bridge, &second, json!([{ "card": card }])).await;
    assert!(seen[0]["marks"].get("rival").is_none());
    assert!(seen[0]["marks"].get("opponents").is_none());
}

/// Players 3 to `last`, with Discord accounts 300 to `last`00.
async fn more_players(bridge: &Bridge, last: u8) {
    for byte in 3..=last {
        let player = player(bridge, byte).await;
        connect_discord(bridge, &format!("{byte}00"), &player).await;
    }
}

/// The record of the pair `first` < `second`, as the games leave it.
async fn pair(
    bridge: &Bridge,
    first: &str,
    second: &str,
    first_wins: u64,
    second_wins: u64,
    played_at: u64,
) {
    let (first, second) = (first.to_owned(), second.to_owned());
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO pair_records (first, second, first_wins, second_wins, played_at)
                 VALUES (?1, ?2, ?3, ?4, ?5)",
                rusqlite::params![first, second, first_wins, second_wins, played_at],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

/// A `player_cards` row for `account`: the name and flag its card shows.
async fn card_row(bridge: &Bridge, account: &str, name: &str, country: Option<&str>) {
    let (account, name, country) =
        (account.to_owned(), name.to_owned(), country.map(str::to_owned));
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO player_cards (discord_user_id, name, country, since, updated_at)
                 VALUES (?1, ?2, ?3, 1, 1)",
                rusqlite::params![account, name, country],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

/// `blocker` blocks `blocked`, as the friends routes store it.
async fn block(bridge: &Bridge, blocker: &str, blocked: &str) {
    let (blocker, blocked) = (blocker.to_owned(), blocked.to_owned());
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO friend_blocks (blocker, blocked, id, name, at)
                 VALUES (?1, ?2, ?3, 'Blocked', 1)",
                rusqlite::params![blocker, blocked, format!("bl_{blocker}_{blocked}")],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

/// `GET /v1/points/me` for `player` as the bridge sent it, checked as the
/// protocol checks it.
async fn points(bridge: &Bridge, player: &Player) -> Json_ {
    let (status, levels) = send(
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
    levels
}

/// The most played opponent in `player`'s own answer.
async fn rival_of(bridge: &Bridge, player: &Player) -> Option<Json_> {
    points(bridge, player).await.get("rival").cloned()
}

/// The name of `player`'s most played opponent, and their record.
async fn rival_name(bridge: &Bridge, player: &Player) -> Option<(String, u64, u64)> {
    rival_of(bridge, player).await.map(|rival| {
        (
            rival["name"].as_str().unwrap().to_owned(),
            rival["won"].as_u64().unwrap(),
            rival["lost"].as_u64().unwrap(),
        )
    })
}

#[tokio::test]
async fn the_most_played_opponent_is_the_pair_with_the_most_games_and_ties_go_to_the_latest() {
    let (bridge, _fake, room_id, first, second) = two_players().await;
    more_players(&bridge, 4).await;
    card_row(&bridge, "200", "Player 2", None).await;
    card_row(&bridge, "300", "Player 3", Some("US")).await;
    card_row(&bridge, "400", "Player 4", None).await;
    // 15 games with 200, 15 with 300 (the later), 4 with 400.
    pair(&bridge, "100", "200", 10, 5, 100).await;
    pair(&bridge, "100", "300", 6, 9, 200).await;
    pair(&bridge, "100", "400", 3, 1, 300).await;
    let rival = rival_of(&bridge, &first).await.expect("a most played opponent");
    assert_eq!(rival["name"], json!("Player 3"));
    assert_eq!(rival["country"], json!("US"));
    assert_eq!((rival["won"].clone(), rival["lost"].clone()), (json!(6), json!(9)));

    // The token opens that player's card, whose record is the same one.
    let token = rival["card"].as_str().unwrap();
    assert!(is_card_token(token), "{token}");
    let seen = cards(&bridge, &first, json!([{ "card": token }])).await;
    assert_eq!(seen[0]["name"], json!("Player 3"));
    assert_eq!(
        (seen[0]["versus"]["won"].clone(), seen[0]["versus"]["lost"].clone()),
        (json!(6), json!(9))
    );
    assert_eq!(rival.get("badge"), seen[0].get("badge"));

    // The pair played last wins a tie; more games win outright.
    sql(
        &bridge,
        "UPDATE pair_records SET played_at = 400 WHERE first = '100' AND second = '200'",
    )
    .await;
    assert_eq!(rival_name(&bridge, &first).await, Some(("Player 2".into(), 10, 5)));
    sql(
        &bridge,
        "UPDATE pair_records SET first_wins = 40 WHERE first = '100' AND second = '400'",
    )
    .await;
    assert_eq!(rival_name(&bridge, &first).await, Some(("Player 4".into(), 40, 1)));

    // A card never names the player's own most played opponent.
    let own_card = token_of(&bridge, &first, &room_id).await;
    let seen = cards(&bridge, &second, json!([{ "card": own_card }])).await;
    assert!(seen[0].get("rival").is_none(), "{}", seen[0]);
}

/// The card token in `player`'s ticket for `room_id`.
async fn token_of(bridge: &Bridge, player: &Player, room_id: &str) -> String {
    token(bridge, player, room_id).await
}

#[tokio::test]
async fn the_record_is_the_callers_whichever_side_of_the_pair_row_they_are() {
    let (bridge, _fake, _room_id, first, second) = two_players().await;
    card_row(&bridge, "100", "Player 1", None).await;
    card_row(&bridge, "200", "Player 2", None).await;
    pair(&bridge, "100", "200", 10, 5, 1).await;
    // The first of the row won 10 and lost 5; the second the other way.
    assert_eq!(rival_name(&bridge, &first).await, Some(("Player 2".into(), 10, 5)));
    assert_eq!(rival_name(&bridge, &second).await, Some(("Player 1".into(), 5, 10)));

    // The same through the games themselves: the first beats the second three
    // times, and each sees it from their own side.
    let (bridge, fake, room_id, first, second) = two_players().await;
    card_row(&bridge, "100", "Player 1", None).await;
    card_row(&bridge, "200", "Player 2", None).await;
    let games = (1..=3).map(|n| game(n, 0, &first, &second, None)).collect();
    play(&bridge, &fake, &room_id, games).await;
    assert_eq!(rival_name(&bridge, &first).await, Some(("Player 2".into(), 3, 0)));
    assert_eq!(rival_name(&bridge, &second).await, Some(("Player 1".into(), 0, 3)));
}

#[tokio::test]
async fn a_pair_where_either_side_blocked_the_other_is_passed_over() {
    let (bridge, _fake, _room_id, first, _second) = two_players().await;
    more_players(&bridge, 4).await;
    for (account, name) in [("200", "Player 2"), ("300", "Player 3"), ("400", "Player 4")] {
        card_row(&bridge, account, name, None).await;
    }
    pair(&bridge, "100", "200", 12, 8, 1).await;
    pair(&bridge, "100", "300", 5, 5, 2).await;
    pair(&bridge, "100", "400", 3, 2, 3).await;
    assert_eq!(rival_name(&bridge, &first).await, Some(("Player 2".into(), 12, 8)));

    // Player 2 blocked the caller: the next most played.
    block(&bridge, "200", "100").await;
    assert_eq!(rival_name(&bridge, &first).await, Some(("Player 3".into(), 5, 5)));
    // The caller blocked Player 3: the one after.
    block(&bridge, "100", "300").await;
    assert_eq!(rival_name(&bridge, &first).await, Some(("Player 4".into(), 3, 2)));
    // Player 4 blocked the caller too: none, though the answer stays counted.
    block(&bridge, "400", "100").await;
    let levels = points(&bridge, &first).await;
    assert_eq!(levels["points"], json!("counted"));
    assert!(levels.get("rival").is_none(), "{levels}");
    // A block that is lifted passes nothing over.
    sql(&bridge, "DELETE FROM friend_blocks WHERE blocker = '200'").await;
    assert_eq!(rival_name(&bridge, &first).await, Some(("Player 2".into(), 12, 8)));
}

#[tokio::test]
async fn an_opponent_with_no_card_row_is_passed_over_and_only_the_first_five_are_looked_at() {
    let (bridge, _fake, _room_id, first, _second) = two_players().await;
    more_players(&bridge, 7).await;
    // Six opponents, 30 down to 5 games; only the sixth has a card.
    for (account, wins, at) in [
        ("200", 30, 1),
        ("300", 25, 2),
        ("400", 20, 3),
        ("500", 15, 4),
        ("600", 10, 5),
        ("700", 5, 6),
    ] {
        pair(&bridge, "100", account, wins, 0, at).await;
    }
    card_row(&bridge, "700", "Player 7", None).await;
    let levels = points(&bridge, &first).await;
    assert_eq!(levels["points"], json!("counted"));
    assert!(levels.get("rival").is_none(), "{levels}");

    // A card for the fifth: it is the first that can be shown.
    card_row(&bridge, "600", "Player 6", None).await;
    assert_eq!(rival_name(&bridge, &first).await, Some(("Player 6".into(), 10, 0)));
    // And for the first: the most played shows once it has a name.
    card_row(&bridge, "200", "Player 2", None).await;
    assert_eq!(rival_name(&bridge, &first).await, Some(("Player 2".into(), 30, 0)));
}

#[tokio::test]
async fn there_is_no_most_played_opponent_before_a_counted_game() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    card_row(&bridge, "100", "Player 1", None).await;
    card_row(&bridge, "200", "Player 2", None).await;
    for caller in [&first, &second] {
        let levels = points(&bridge, caller).await;
        assert_eq!(levels["points"], json!("counted"));
        assert!(levels.get("rival").is_none(), "{levels}");
    }
    // The first counted game brings one.
    play(&bridge, &fake, &room_id, vec![game(1, 0, &second, &first, None)]).await;
    assert_eq!(rival_name(&bridge, &first).await, Some(("Player 2".into(), 0, 1)));
    // A player with no Discord account has no points answer to hold one.
    let unlinked = player(&bridge, 9).await;
    let levels = points(&bridge, &unlinked).await;
    assert_eq!(levels, json!({ "points": "no_account" }));
}

#[tokio::test]
async fn the_most_played_opponent_names_no_account() {
    let (bridge, _fake, _room_id, first, second) = two_players().await;
    card_row(&bridge, "200", "Player 2", Some("US")).await;
    pair(&bridge, "100", "200", 4, 2, 1).await;
    let levels = points(&bridge, &first).await;
    let rival = levels["rival"].as_object().expect("a most played opponent");
    assert!(
        rival
            .keys()
            .all(|member| ["badge", "card", "country", "lost", "name", "won"].contains(&member.as_str())),
        "{rival:?}"
    );
    let text = levels.to_string();
    assert!(!text.contains("\"200\""), "{text}");
    assert!(!text.contains(second.id().as_str()), "{text}");
    // The opponent is named by a token, and no token reads as the account.
    assert_ne!(rival["card"], json!("c1.200"));
}
