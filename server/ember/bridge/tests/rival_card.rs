//! The most played opponent of a player's own points answer: the pair with
//! the most games, ties to the latest, passed over for a block either way or
//! when it has no card to name, and never naming an account.
mod common;

use common::{
    Bridge, Player,
    points::*,
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
