//! The card look-up of the profile (docs/design/PROFILE.md): a received request and a friend code
//! open a card, which says the caller's record and friendship with its player, what a friends
//! sync keeps of it, and the class it shows.

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
async fn a_look_up_by_request_opens_only_the_receivers_visible_request() {
    let world = World::start().await;
    let a = world.joined(1, 1, "Player 1").await;
    let b = world.joined(2, 2, "Player 2").await;
    let c = world.joined(3, 3, "Player 3").await;
    world.save(&b, json!({ "listing": { "name": "Player 2" }, "main": 7 })).await;

    // B asks A: A holds the request, and opens B's card with it.
    let request = world.ask(&b, &a).await;
    let card = world.cards(&a, json!([{ "request": request }])).await;
    assert_eq!(card[0]["name"], "Player 2");
    assert_eq!(card[0]["main"], 7);
    assert_eq!(card[0]["relation"], "received");
    // Nobody else holds it: not the sender, not a stranger.
    assert_eq!(
        world.cards(&b, json!([{ "request": request }])).await,
        json!([null])
    );
    assert_eq!(
        world.cards(&c, json!([{ "request": request }])).await,
        json!([null])
    );
    // A request nobody sent, and a friend id, are no request.
    assert_eq!(
        world
            .cards(&a, json!([{ "request": "rq_11111111-1111-4111-8111-111111111111" }]))
            .await,
        json!([null])
    );
    // A declined request is hidden from its receiver, and opens nothing.
    world
        .act(&a, json!({ "action": "decline", "request": request }))
        .await;
    assert_eq!(
        world.cards(&a, json!([{ "request": request }])).await,
        json!([null])
    );
}

#[tokio::test]
async fn a_look_up_by_code_opens_the_card_of_its_one_account() {
    let world = World::start().await;
    let a = world.joined(1, 1, "Player 1").await;
    let b = world.joined(2, 2, "Player 2").await;
    let code = world.code_of(&b).await;
    let typed = code.to_lowercase().replace('0', "o");

    let cards = world
        .cards(
            &a,
            json!([{ "code": code }, { "code": typed }, { "code": world.code_of(&a).await },
                   { "code": "ZZZZ-ZZZZ" }]),
        )
        .await;
    assert_eq!(cards[0]["name"], "Player 2");
    assert_eq!(cards[0]["relation"], "none");
    assert_eq!(cards[0], cards[1]);
    // Never the caller's own, and never a code nobody holds.
    assert_eq!(cards[2], Json_::Null);
    assert_eq!(cards[3], Json_::Null);

    // A player who blocked the caller shows nothing, found by code or not.
    world
        .sql(format!(
            "INSERT INTO friend_blocks (blocker, blocked, id, name, at) VALUES ('{}', '{}', 'bl_x', 'a', 1)",
            b.discord, a.discord
        ))
        .await;
    assert_eq!(
        world.cards(&a, json!([{ "code": code }])).await,
        json!([null])
    );
}

/// Both orders of two accounts' pair record, as the bridge stores one: the
/// first account is the one that sorts first.
async fn pair(world: &World, one: &Member, other: &Member, one_wins: u64, other_wins: u64) {
    let (first, second, first_wins, second_wins) = if one.discord < other.discord {
        (&one.discord, &other.discord, one_wins, other_wins)
    } else {
        (&other.discord, &one.discord, other_wins, one_wins)
    };
    world
        .sql(format!(
            "INSERT OR REPLACE INTO pair_records (first, second, first_wins, second_wins, played_at)
             VALUES ('{first}', '{second}', {first_wins}, {second_wins}, 1731000000)"
        ))
        .await;
}

#[tokio::test]
async fn a_card_says_the_callers_record_against_its_player_from_the_callers_side() {
    let world = World::start().await;
    let a = world.member(1, 1, "Player 1").await;
    let b = world.member(2, 2, "Player 2").await;
    let c = world.member(3, 3, "Player 3").await;
    // Before a counted game there is no record to say.
    assert!(world.card_of(&a, &b).await.get("versus").is_none());

    pair(&world, &a, &b, 7, 3).await;
    assert_eq!(
        world.card_of(&a, &b).await["versus"],
        json!({ "won": 7, "lost": 3, "last_at": 1731000000 })
    );
    assert_eq!(
        world.card_of(&b, &a).await["versus"],
        json!({ "won": 3, "lost": 7, "last_at": 1731000000 })
    );
    // Anyone else sees their own record, here none; a player's own card
    // has no record against themselves.
    assert!(world.card_of(&c, &b).await.get("versus").is_none());
    assert!(world.card_of(&a, &a).await.get("versus").is_none());
    // A pair of zeros is no record.
    pair(&world, &a, &c, 0, 0).await;
    assert!(world.card_of(&a, &c).await.get("versus").is_none());
}

#[tokio::test]
async fn a_friends_card_names_the_friendship_and_nobody_elses_does() {
    let world = World::start().await;
    let a = world.joined(1, 1, "Player 1").await;
    let b = world.joined(2, 2, "Player 2").await;
    let c = world.joined(3, 3, "Player 3").await;
    let friend = world.befriend(&a, &b).await;

    let card = world.card_of(&a, &b).await;
    assert_eq!(card["relation"], "friend");
    assert_eq!(card["friend"], friend.as_str());
    // The same friendship from the other side, and by its id.
    assert_eq!(world.card_of(&b, &a).await["friend"], friend.as_str());
    let by_id = world.cards(&a, json!([{ "friend": friend }])).await;
    assert_eq!(by_id[0]["friend"], friend.as_str());
    // Someone who is not their friend sees none of it.
    let card = world.card_of(&c, &b).await;
    assert_eq!(card["relation"], "none");
    assert!(card.get("friend").is_none());
    // After the friendship ends the card names none.
    world
        .act(&a, json!({ "action": "remove", "friend": friend }))
        .await;
    assert!(world.card_of(&a, &b).await.get("friend").is_none());
}

#[tokio::test]
async fn a_friends_sync_keeps_the_card_current_and_leaves_what_was_saved() {
    let world = World::start().await;
    let a = world.member(1, 1, "Player 1").await;
    let b = world.member(2, 2, "Player 2").await;
    // A game from before cards only syncs: its card is the listing it sent.
    world.sync_as(&a, "Player 1", "GB", 3).await;
    let card = world.card_of(&b, &a).await;
    assert_eq!(card["name"], "Player 1");
    assert_eq!(card["country"], "GB");
    assert_eq!(card["main"], 3);
    assert_eq!(card["since"], MONTH);
    assert!(card.get("style").is_none());

    // What it sends later replaces the three, in any case.
    world.tick();
    world.sync_as(&a, "player 1", "FR", 5).await;
    let card = world.card_of(&b, &a).await;
    assert_eq!(card["name"], "player 1");
    assert_eq!((card["country"].clone(), card["main"].clone()), (json!("FR"), json!(5)));

    // A saved card keeps what only a save writes through the syncs after.
    world.save(&a, full_card()).await;
    world.tick();
    world.sync_as(&a, "Player 1", "DE", 3).await;
    let card = world.card_of(&b, &a).await;
    assert_eq!(card["worn"], json!({ "title": "matches", "icon": "streak" }));
    assert_eq!(card["style"]["device"], 2);
    assert_eq!(card["since"], MONTH);

    // A sync that changes nothing writes nothing: the card is no newer.
    let before = world.stored(&a.discord).await;
    world.tick();
    world.sync_as(&a, "Player 1", "DE", 3).await;
    assert_eq!(world.stored(&a.discord).await, before);

    // An account that turned friends off is not written by its game's syncs.
    // Forgetting answers as friends are off, the state it leaves.
    let (forgot, _) = world.call(Method::POST, &a, ACTIONS_PATH, Some(json!({ "action": "forget" }))).await;
    assert_eq!(forgot, StatusCode::CONFLICT);
    let (status, _) = world
        .call(
            Method::POST,
            &a,
            SYNC_PATH,
            Some(json!({ "build_id": BUILD, "listing": { "name": "Changed" },
                "presence": { "activity": "menus" } })),
        )
        .await;
    assert_ne!(status, StatusCode::OK);
    assert_eq!(world.card_of(&b, &a).await["name"], "Player 1");
}

/// A settled rating of class 6 for `account`, as its games would leave it.
async fn seed_rating(world: &World, account: &str) {
    let now = world.now();
    world
        .sql(format!(
            "INSERT INTO skill_ratings
               (game, discord_user_id, rating, deviation, volatility, band, played_at)
             VALUES ('usf4', '{account}', 1650, 80, 0.06, 6, {now})"
        ))
        .await;
}

#[tokio::test]
async fn a_card_shows_the_class_only_while_classes_are_shown_and_the_player_takes_part() {
    let shown = World::start_with(true, true).await;
    let a = shown.member(1, 1, "Player 1").await;
    let b = shown.member(2, 2, "Player 2").await;
    seed_rating(&shown, &a.discord).await;
    shown.save(&a, json!({ "listing": { "name": "Player 1" } })).await;
    assert_eq!(
        shown.card_of(&b, &a).await["badge"],
        json!({ "band": 6, "settled": true })
    );
    let answer = shown.search(&b, json!({ "name": "Player 1" })).await;
    assert_eq!(answer["players"][0]["badge"], json!({ "band": 6, "settled": true }));
    // Not taking part shows no class.
    shown
        .sql(format!(
            "INSERT INTO ranking_choices (game, discord_user_id, ranked) VALUES ('usf4', '{}', 0)",
            a.discord
        ))
        .await;
    assert!(shown.card_of(&b, &a).await.get("badge").is_none());

    // A bridge that hides classes shows none, whoever takes part.
    let hidden = World::start_with(true, false).await;
    let a = hidden.member(1, 1, "Player 1").await;
    let b = hidden.member(2, 2, "Player 2").await;
    seed_rating(&hidden, &a.discord).await;
    assert!(hidden.card_of(&b, &a).await.get("badge").is_none());
    hidden.save(&a, json!({ "listing": { "name": "Player 1" } })).await;
    let answer = hidden.search(&b, json!({ "name": "Player 1" })).await;
    assert!(answer["players"][0].get("badge").is_none());
}

#[tokio::test]
async fn no_answer_of_the_profile_routes_names_an_account() {
    let world = World::start().await;
    let a = world.joined(1, 1, "Player 1").await;
    let b = world.joined(2, 2, "Player 2").await;
    world.save(&a, full_card()).await;
    world.save(&b, full_card()).await;
    let friend = world.befriend(&a, &b).await;
    pair(&world, &a, &b, 2, 1).await;
    let code = world.code_of(&b).await;
    let request = {
        let c = world.joined(3, 3, "Player 3").await;
        world.ask(&c, &a).await
    };
    world.search(&a, json!({ "name": "Player" })).await;
    world.search(&a, json!({ "code": code })).await;
    world
        .cards(
            &a,
            json!([{ "card": world.token(&b) }, { "friend": friend }, { "request": request },
                   { "code": code }]),
        )
        .await;
    // `World`'s drop scans every answer above for the Discord IDs and names
    // and the Ember IDs it seeded; each route was called at least once.
    let paths: std::collections::BTreeSet<String> = world
        .answers
        .lock()
        .unwrap()
        .iter()
        .map(|(path, _)| path.clone())
        .collect();
    for path in [CARDS_ME_PATH, SEARCH_PATH, LOOKUP_PATH] {
        assert!(paths.contains(path), "{path} was not called");
    }
}
