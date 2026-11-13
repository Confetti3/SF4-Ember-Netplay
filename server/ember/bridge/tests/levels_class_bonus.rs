//! The class bonus (docs/design/FIGHTER_LEVELS.md, "Points and tiers"): a
//! counted game is 1 point for each player, and the winner earns 1 more only
//! against an opponent of the same class or a higher one, or when either has
//! no class. The loser earns its point whatever the classes, the account's
//! points and the fighter's agree, the repeat cap still counts nothing, and a
//! build from before the bonus is answered games plus wins.
mod common;

use common::{Bridge, Player, points::*};
use ember_protocol::levels::{CLASS_BONUS_FEATURE, POINTS_FEATURE};
use reqwest::StatusCode;
use serde_json::{Value as Json_, json};

/// Gives Discord account `account` the class `band` as its rating last showed
/// it (0: a rating still placing), leaving a rating it already has.
async fn set_class(bridge: &Bridge, account: &'static str, band: u8) {
    let now = bridge.state().now();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO skill_ratings
                   (game, discord_user_id, rating, deviation, volatility, band, played_at)
                 VALUES ('usf4', ?1, 1500, 80, 0.06, ?2, ?3)
                 ON CONFLICT (game, discord_user_id) DO UPDATE SET band = ?2",
                rusqlite::params![account, band, now],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

/// The (points, games, wins) of `player`'s account, and of its fighter
/// `fighter`, as their own answer has them.
async fn counts(bridge: &Bridge, player: &Player, fighter: u8) -> [(u64, u64, u64); 2] {
    let levels = own(bridge, player).await;
    let of = |count: &Json_| {
        (
            count["points"].as_u64().unwrap(),
            count["games"].as_u64().unwrap(),
            count["wins"].as_u64().unwrap(),
        )
    };
    let fighter = levels["fighters"]
        .as_array()
        .unwrap()
        .iter()
        .find(|row| row["fighter"] == json!(fighter))
        .expect("the fighter is played");
    [of(&levels["account"]), of(fighter)]
}

#[tokio::test]
async fn a_win_earns_its_bonus_only_against_the_same_class_or_a_higher_one() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    set_class(&bridge, "100", 6).await;
    // The same class, a higher one, then a lower one: 2, 2 and 1 for the
    // winner, and 1 each time for the loser.
    for (generation, class, winner_points, loser_points) in [(1, 6, 2, 1), (2, 8, 4, 2), (3, 4, 5, 3)] {
        set_class(&bridge, "200", class).await;
        play(
            &bridge,
            &fake,
            &room_id,
            vec![game(generation, 0, &first, &second, Some([3, 7]))],
        )
        .await;
        // The account's points and the fighter's are the same count.
        assert_eq!(
            counts(&bridge, &first, 3).await,
            [(winner_points, generation, generation); 2],
            "against class {class}"
        );
        assert_eq!(
            counts(&bridge, &second, 7).await,
            [(loser_points, generation, 0); 2],
            "against class {class}"
        );
    }
    // The games and wins are exact; what is kept beside them is the one win
    // that earned no bonus.
    assert_eq!(account_points(&bridge, "100").await, Some((3, 3)));
    assert_eq!(fighters(&bridge, "100").await, vec![(3, 3, 3)]);
    assert_eq!(lower_wins(&bridge, "100").await, (1, vec![(3, 1)]));
    assert_eq!(lower_wins(&bridge, "200").await, (0, vec![(7, 0)]));

    // The lower class beats the higher one: a full 2 for the winner, and the
    // loser's point all the same.
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(4, 0, &second, &first, Some([7, 3]))],
    )
    .await;
    assert_eq!(counts(&bridge, &second, 7).await, [(5, 4, 1); 2]);
    assert_eq!(counts(&bridge, &first, 3).await, [(6, 4, 3); 2]);

    // A card shows the same points as the player's own answer.
    let card = token(&bridge, &first, &room_id).await;
    let seen = cards(&bridge, &second, json!([{ "card": card }])).await;
    assert_eq!(seen[0]["account"], json!({ "points": 6, "tier": 0 }));
    assert_eq!(
        seen[0]["fighters"],
        json!([{ "fighter": 3, "points": 6, "tier": 0 }])
    );
    assert_eq!(seen[0]["awards"]["games"], json!(4));
    assert_eq!(seen[0]["awards"]["wins"], json!(3));
}

#[tokio::test]
async fn a_player_without_a_class_is_nobodys_lower() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    // Neither has a rating yet; then the winner has a class and the loser
    // none; then the loser's rating is still placing (class 0); then the
    // winner has none and the loser the lowest class. Each win is 2.
    let steps: [(Option<u8>, Option<u8>); 4] =
        [(None, None), (Some(9), None), (Some(9), Some(0)), (Some(0), Some(1))];
    for (index, (winner, loser)) in steps.into_iter().enumerate() {
        let generation = index as u64 + 1;
        if let Some(class) = winner {
            set_class(&bridge, "100", class).await;
        }
        if let Some(class) = loser {
            set_class(&bridge, "200", class).await;
        }
        play(
            &bridge,
            &fake,
            &room_id,
            vec![game(generation, 0, &first, &second, Some([3, 7]))],
        )
        .await;
        assert_eq!(
            counts(&bridge, &first, 3).await,
            [(2 * generation, generation, generation); 2],
            "game {generation}"
        );
    }
    assert_eq!(lower_wins(&bridge, "100").await, (0, vec![(3, 0)]));
    // Once both have a class the lower one is known, and the win is 1.
    set_class(&bridge, "100", 9).await;
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(5, 0, &first, &second, Some([3, 7]))],
    )
    .await;
    assert_eq!(counts(&bridge, &first, 3).await, [(9, 5, 5); 2]);
}

#[tokio::test]
async fn the_loser_earns_a_point_whatever_the_classes_and_no_points_are_taken() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    // The top class against the lowest.
    set_class(&bridge, "100", 10).await;
    set_class(&bridge, "200", 1).await;
    let mut before = [0, 0];
    // The top class loses to the lowest three times, then wins twice.
    for (generation, top_won) in [(1, false), (2, false), (3, false), (4, true), (5, true)] {
        let (winner, loser, picks) = if top_won {
            (&first, &second, [3, 7])
        } else {
            (&second, &first, [7, 3])
        };
        play(
            &bridge,
            &fake,
            &room_id,
            vec![game(generation, 0, winner, loser, Some(picks))],
        )
        .await;
        let after = [
            counts(&bridge, &first, 3).await[0].0,
            counts(&bridge, &second, 7).await[0].0,
        ];
        // The top class earns 1 either way: its loss is a point, and its win
        // against a lower class is no more. The lowest class earns 2 for
        // its win and 1 for its loss.
        let earned = [after[0] - before[0], after[1] - before[1]];
        assert_eq!(earned, if top_won { [1, 1] } else { [1, 2] }, "game {generation}");
        before = after;
    }
    assert_eq!(counts(&bridge, &first, 3).await, [(5, 5, 2); 2]);
    assert_eq!(counts(&bridge, &second, 7).await, [(8, 5, 3); 2]);
}

#[tokio::test]
async fn past_eight_games_in_a_sitting_neither_the_point_nor_the_bonus_is_counted() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    set_class(&bridge, "100", 8).await;
    set_class(&bridge, "200", 3).await;
    // Eight wins against a lower class are 8 points, and the ninth nothing.
    let sitting: Vec<Json_> = (1..=9)
        .map(|generation| game(generation, 0, &first, &second, Some([3, 7])))
        .collect();
    play(&bridge, &fake, &room_id, sitting).await;
    assert_eq!(counts(&bridge, &first, 3).await, [(8, 8, 8); 2]);
    assert_eq!(counts(&bridge, &second, 7).await, [(8, 8, 0); 2]);
    assert_eq!(lower_wins(&bridge, "100").await, (8, vec![(3, 8)]));
    // Nor does a win of the lower class past the cap, which would be 2.
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(10, 0, &second, &first, Some([7, 3]))],
    )
    .await;
    assert_eq!(counts(&bridge, &first, 3).await, [(8, 8, 8); 2]);
    assert_eq!(counts(&bridge, &second, 7).await, [(8, 8, 0); 2]);

    // A new sitting counts again, by the same rule: the lower class's win
    // with its bonus, the higher class's loss as a game. The players'
    // sessions have ended by then, so the stored counts are read.
    bridge.clock.advance(3 * 60 * 60 + 1);
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(11, 0, &second, &first, Some([7, 3]))],
    )
    .await;
    assert_eq!(account_points(&bridge, "200").await, Some((9, 1)));
    assert_eq!(fighters(&bridge, "200").await, vec![(7, 9, 1)]);
    assert_eq!(lower_wins(&bridge, "200").await, (0, vec![(7, 0)]));
    assert_eq!(account_points(&bridge, "100").await, Some((9, 8)));
    assert_eq!(fighters(&bridge, "100").await, vec![(3, 9, 8)]);
    assert_eq!(lower_wins(&bridge, "100").await, (8, vec![(3, 8)]));
}

/// Gives Discord account `account` a settled rating of `rating`, with no
/// class shown for it yet.
async fn settle_rating(bridge: &Bridge, account: &'static str, rating: f64) {
    let now = bridge.state().now();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO skill_ratings
                   (game, discord_user_id, rating, deviation, volatility, band, played_at)
                 VALUES ('usf4', ?1, ?2, 80, 0.06, 0, ?3)",
                rusqlite::params![account, rating, now],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

#[tokio::test]
async fn the_class_a_ticket_showed_is_the_one_the_bonus_reads() {
    let (bridge, fake, room_id, first, second) = two_players_showing_classes().await;
    // A Grand Master's rating and a D player's, neither shown a class yet:
    // until a ticket shows one, neither is lower.
    settle_rating(&bridge, "100", 2100.0).await;
    settle_rating(&bridge, "200", 1150.0).await;
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(1, 0, &first, &second, Some([3, 7]))],
    )
    .await;
    assert_eq!(counts(&bridge, &first, 3).await, [(2, 1, 1); 2]);

    // Both join through a ticket, which shows each their class. The Grand
    // Master's win is then 1 point, and the D player's 2.
    ticket(&bridge, &first, &room_id, true).await;
    ticket(&bridge, &second, &room_id, true).await;
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(2, 0, &first, &second, Some([3, 7]))],
    )
    .await;
    assert_eq!(counts(&bridge, &first, 3).await, [(3, 2, 2); 2]);
    assert_eq!(counts(&bridge, &second, 7).await, [(2, 2, 0); 2]);
    play(
        &bridge,
        &fake,
        &room_id,
        vec![game(3, 0, &second, &first, Some([7, 3]))],
    )
    .await;
    assert_eq!(counts(&bridge, &second, 7).await, [(4, 3, 1); 2]);
    assert_eq!(counts(&bridge, &first, 3).await, [(4, 3, 2); 2]);
}

#[tokio::test]
async fn a_build_from_before_the_bonus_is_answered_games_plus_wins() {
    let (bridge, fake, room_id, first, second) = two_players().await;
    // The bridge lists the class bonus beside points.
    let response = bridge
        .client
        .get(bridge.url("/v1/capabilities"))
        .send()
        .await
        .unwrap();
    let (status, body) = common::read(response).await;
    assert_eq!(status, StatusCode::OK, "{body}");
    let features = body["features"].as_array().unwrap();
    assert!(features.contains(&json!(POINTS_FEATURE)), "{body}");
    assert!(features.contains(&json!(CLASS_BONUS_FEATURE)), "{body}");

    set_class(&bridge, "100", 8).await;
    set_class(&bridge, "200", 3).await;
    play(
        &bridge,
        &fake,
        &room_id,
        vec![
            game(1, 0, &first, &second, Some([3, 7])),
            game(2, 0, &first, &second, Some([3, 7])),
            game(3, 0, &second, &first, Some([7, 3])),
        ],
    )
    .await;
    // As earned: two wins against a lower class and a loss are 3 points.
    assert_eq!(
        own(&bridge, &first).await,
        json!({ "points": "counted",
            "account": { "points": 3, "games": 3, "wins": 2, "tier": 0, "floor": 0, "next": 500 },
            "fighters": [
            { "fighter": 3, "points": 3, "games": 3, "wins": 2, "tier": 0, "floor": 0, "next": 200 }] })
    );
    // Asked with no query, as a build from before the bonus asks: the same
    // games and wins, and the points that build checks them against.
    let before = own_before_the_bonus(&bridge, &first).await;
    assert_eq!(
        before,
        json!({ "points": "counted",
            "account": { "points": 5, "games": 3, "wins": 2, "tier": 0, "floor": 0, "next": 500 },
            "fighters": [
            { "fighter": 3, "points": 5, "games": 3, "wins": 2, "tier": 0, "floor": 0, "next": 200 }] })
    );
    // A player whose every win earned its bonus reads the same either way.
    assert_eq!(
        own(&bridge, &second).await,
        own_before_the_bonus(&bridge, &second).await
    );
    assert_eq!(counts(&bridge, &second, 7).await, [(4, 3, 1); 2]);

    // The tier is that of the points answered: 150 games with 100 wins, 60
    // of them without a bonus, are 190 points (Ash) as earned, and 250
    // (Copper) to a build from before the bonus.
    sql(
        &bridge,
        "UPDATE fighter_levels SET games = 150, wins = 100, lower_wins = 60
         WHERE discord_user_id = '100' AND fighter = 3",
    )
    .await;
    let fighter = |levels: Json_| levels["fighters"][0].clone();
    assert_eq!(
        fighter(own(&bridge, &first).await),
        json!({ "fighter": 3, "points": 190, "games": 150, "wins": 100, "tier": 0, "floor": 0, "next": 200 })
    );
    assert_eq!(
        fighter(own_before_the_bonus(&bridge, &first).await),
        json!({ "fighter": 3, "points": 250, "games": 150, "wins": 100, "tier": 1, "floor": 200, "next": 700 })
    );
}
