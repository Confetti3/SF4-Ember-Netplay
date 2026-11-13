//! Award counters (`src/netplay/Awards.hxx`): the best run of wins only rises
//! and a loss ends a run, the peak class never drops, and the rival and
//! opponent counts come from the pair records. A card shows them as
//! `AwardCounts::shown`.
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
        seen[0]["awards"],
        json!({ "games": 6, "wins": 5, "best_streak": 3 })
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
    let class = |seen: &Json_| seen[0]["awards"]["peak_class"].as_u64();
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
    assert!(seen[0]["awards"].get("rival").is_none());
    assert!(seen[0]["awards"].get("opponents").is_none());
}
