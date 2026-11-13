//! Profiles (docs/design/PROFILE.md): a player saves what their card shows, and the bridge offers
//! the routes beside points. The helpers are in `common/profiles.rs`; Migration 064's test is in
//! `levels_migrations.rs`.

mod common;

use common::{
    Bridge, Player,
    profiles::*,
    public_rooms::{player, reason, send},
    seed,
    supervisor::{self, BUILD, Supervisor},
};
use ember_bridge::{config, integrations::Secrets};
use ember_protocol::{
    encoding::b64u,
    friends::{ACTIONS_PATH, SYNC_PATH},
    levels::{
        CARD_PREFIX, CARDS_ME_PATH, CardAnswers, LOOKUP_PATH, MAX_SEARCH, SEARCH_PATH,
        SearchAnswer, is_card_token,
    },
};
use reqwest::{Method, StatusCode};
use rusqlite::OptionalExtension;
use serde_json::{Value as Json_, json};
use zeroize::Zeroizing;

#[tokio::test]
async fn profiles_are_offered_beside_points_where_public_rooms_are_on() {
    let world = World::start().await;
    assert!(offers(&world.bridge, "profiles").await);
    assert!(offers(&world.bridge, "points").await);

    // A bridge without public rooms has neither, and the routes are unknown.
    let plain = Bridge::start().await;
    assert!(!offers(&plain, "profiles").await);
    assert!(!offers(&plain, "points").await);
    let caller = player(&plain, 1).await;
    let (status, _) = send(&plain, Method::PUT, caller.token(), IP, CARDS_ME_PATH, Some(full_card())).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = send(
        &plain,
        Method::POST,
        caller.token(),
        IP,
        SEARCH_PATH,
        Some(json!({ "name": "Player" })),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
}

#[tokio::test]
async fn a_card_shows_what_its_player_saved_to_anyone_with_a_selector() {
    let world = World::start().await;
    let a = world.member(1, 1, "Player 1").await;
    let b = world.member(2, 2, "Player 2").await;

    // Before a save a card is its points and nothing of the player's.
    let bare = world.card_of(&b, &a).await;
    assert_eq!(
        bare,
        json!({ "account": { "points": 0, "tier": 0 }, "fighters": [], "relation": "none" })
    );

    world.save(&a, full_card()).await;
    let card = world.card_of(&b, &a).await;
    assert_eq!(card["name"], "Player 1");
    assert_eq!(card["country"], "DE");
    assert_eq!(card["main"], 3);
    assert_eq!(card["worn"], json!({ "title": "matches", "icon": "streak" }));
    assert_eq!(card["pins"], json!(["rank", "wins"]));
    assert_eq!(
        card["style"],
        json!({ "device": 2, "link": 1, "region": "euc1", "looking": 34 })
    );
    // The month they came, never the day, and never an account.
    assert_eq!(card["since"], MONTH);
    for member in ["discord", "ember", "rating", "code"] {
        assert!(!card.to_string().contains(member), "{member}: {card}");
    }
    // A player sees their own card the same way.
    assert_eq!(world.card_of(&a, &a).await["name"], "Player 1");

    // A save replaces the whole card: what it leaves out is gone, and the
    // month they came never moves, not even when Discord is connected again.
    world
        .sql(format!(
            "UPDATE discord_accounts SET connected_at = 1800000000 WHERE user_id = '{}'",
            a.discord
        ))
        .await;
    world
        .save(&a, json!({ "listing": { "name": "Player 1 again" } }))
        .await;
    let card = world.card_of(&b, &a).await;
    assert_eq!(card["name"], "Player 1 again");
    for gone in ["country", "main", "worn", "pins", "style"] {
        assert!(card.get(gone).is_none(), "{gone}: {card}");
    }
    assert_eq!(card["since"], MONTH);

    // A card made after the account's latest connection says that month.
    let c = world.member(3, 3, "Player 3").await;
    world
        .sql(format!(
            "UPDATE discord_accounts SET connected_at = 1800000000 WHERE user_id = '{}'",
            c.discord
        ))
        .await;
    world.save(&c, json!({ "listing": { "name": "Player 3" } })).await;
    // 2027-01-15, so the first of that January.
    assert_eq!(world.card_of(&a, &c).await["since"], 1_798_761_600);
}

#[tokio::test]
async fn a_save_that_is_not_one_the_bridge_stores_is_refused() {
    let world = World::start().await;
    let a = world.member(1, 1, "Player 1").await;
    let b = world.member(2, 2, "Player 2").await;
    let with = |change: &dyn Fn(&mut Json_)| {
        let mut card = full_card();
        change(&mut card);
        card
    };
    let bad = [
        with(&|card| card["pins"] = json!(["a", "b", "c", "d", "e"])),
        with(&|card| card["pins"] = json!(["rank", "rank"])),
        with(&|card| card["pins"] = json!(["Rank"])),
        with(&|card| card["pins"] = json!([""])),
        with(&|card| card["pins"] = json!(["a".repeat(33)])),
        with(&|card| card["worn"] = json!({ "title": "no_underscores" })),
        with(&|card| card["style"]["looking"] = json!(7)),
        with(&|card| card["style"]["looking"] = json!(64)),
        with(&|card| card["style"]["device"] = json!(5)),
        with(&|card| card["style"]["link"] = json!(3)),
        with(&|card| card["style"]["region"] = json!("mars")),
        with(&|card| card["main"] = json!(44)),
        with(&|card| card["listing"]["name"] = json!("")),
        with(&|card| card["listing"]["country"] = json!("de")),
        with(&|card| card["extra"] = json!(1)),
        json!({}),
    ];
    // Each refused save is spent from the window, and stores nothing.
    for (at, card) in bad.iter().enumerate() {
        let member = if at < 8 { &a } else { &b };
        let (status, answer) = world.try_save(member, card.clone()).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{card}: {answer}");
    }
    assert!(world.stored(&a.discord).await.is_none());
    assert!(world.stored(&b.discord).await.is_none());

    // The most it takes is accepted: four pins, two bits, the last device and
    // link, every known region.
    for (at, region) in ember_protocol::relay::REGIONS.into_iter().enumerate() {
        let mut card = with(&|card| {
            card["pins"] = json!(["a", "b", "c", "d"]);
            card["style"] = json!({ "device": 4, "link": 2, "looking": 33 });
            card["main"] = json!(43);
        });
        card["style"]["region"] = json!(region);
        world.save(if at < 2 { &a } else { &b }, card).await;
        let seen = world.card_of(&b, &a).await;
        assert_eq!(seen["pins"], json!(["a", "b", "c", "d"]));
    }
}

#[tokio::test]
async fn saves_and_searches_are_limited_and_need_a_discord_account() {
    let world = World::start().await;
    let a = world.member(1, 1, "Player 1").await;
    for _ in 0..10 {
        world.save(&a, full_card()).await;
    }
    let (status, answer) = world.try_save(&a, full_card()).await;
    assert_eq!(status, StatusCode::TOO_MANY_REQUESTS, "{answer}");
    for _ in 0..20 {
        world.search(&a, json!({ "name": "Nobody" })).await;
    }
    let (status, answer) = world.try_search(&a, json!({ "name": "Nobody" })).await;
    assert_eq!(status, StatusCode::TOO_MANY_REQUESTS, "{answer}");

    // The window slides: a minute on, searches again (the session lasts ten).
    world.bridge.clock.advance(61);
    world.search(&a, json!({ "name": "Nobody" })).await;

    // A player without a Discord account has no card to save or search from.
    let lone = Member {
        player: player(&world.bridge, 9).await,
        discord: String::new(),
        name: "Player 9",
    };
    let (status, answer) = world.try_save(&lone, full_card()).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{answer}");
    assert_eq!(reason(&answer), "no_discord_account");
    let (status, answer) = world.try_search(&lone, json!({ "name": "Player" })).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{answer}");
    assert_eq!(reason(&answer), "no_discord_account");
}
