//! Profile search (docs/design/PROFILE.md): players are found by name or friend code, never
//! listing someone who blocked the caller, has no Discord account or turned friends off.

mod common;

use common::{Player, profiles::*, public_rooms::player};
use ember_protocol::{
    friends::ACTIONS_PATH,
    levels::{MAX_SEARCH, is_card_token},
};
use reqwest::{Method, StatusCode};
use serde_json::json;

/// Names saved in this order, a second apart: the most recent is last.
async fn named(
    world: &World,
    names: &[&'static str],
    first: u8,
    first_account: u64,
) -> Vec<Member> {
    let mut members = Vec::new();
    for (at, name) in names.iter().copied().enumerate() {
        let member = world
            .member(first + at as u8, first_account + at as u64, name)
            .await;
        world.tick();
        world
            .save(&member, json!({ "listing": { "name": name } }))
            .await;
        members.push(member);
    }
    members
}

#[tokio::test]
async fn a_name_finds_exact_matches_first_then_leading_ones_each_newest_first() {
    let world = World::start().await;
    let caller = world.member(1, 1, "Annabel").await;
    world
        .save(&caller, json!({ "listing": { "name": "Annabel" } }))
        .await;
    // Saved oldest first. "Joann" holds the text but does not begin with it.
    named(
        &world,
        &["Ann", "Annie", "ann", "Anna", "Joann", "Bo_b", "Bobb"],
        10,
        10,
    )
    .await;

    // Any case finds the same players: the exact names (any case) first, the
    // newest first, then those that begin with it, again the newest first.
    // The caller, who also begins with it, is left out.
    for text in ["ann", "Ann", "ANN", "  ann  "] {
        let (names, more) = world.found(&caller, json!({ "name": text })).await;
        assert_eq!(names, ["ann", "Ann", "Anna", "Annie"], "{text:?}");
        assert!(!more);
        world.bridge.clock.advance(61);
    }
    let (names, _) = world.found(&caller, json!({ "name": "Anni" })).await;
    assert_eq!(names, ["Annie"]);
    // Wildcards are text: `_` is not "any character" and `%` matches nothing.
    let (names, _) = world.found(&caller, json!({ "name": "Bo_" })).await;
    assert_eq!(names, ["Bo_b"]);
    let (names, _) = world.found(&caller, json!({ "name": "Bo%" })).await;
    assert!(names.is_empty());
    let (names, _) = world.found(&caller, json!({ "name": "Zed" })).await;
    assert!(names.is_empty());

    // A hit opens the card of the player it names, and is a card token.
    let answer = world.search(&caller, json!({ "name": "Anni" })).await;
    let hit = &answer["players"][0];
    assert!(is_card_token(hit["card"].as_str().unwrap()));
    assert_eq!(hit["since"], MONTH);
    let card = world.cards(&caller, json!([{ "card": hit["card"] }])).await;
    assert_eq!(card[0]["name"], "Annie");
}

#[tokio::test]
async fn a_search_lists_nobody_who_blocked_the_caller_or_has_no_discord_account() {
    let world = World::start().await;
    let caller = world.member(1, 1, "Player 1").await;
    world
        .save(&caller, json!({ "listing": { "name": "Player 1" } }))
        .await;
    let others = named(&world, &["Rex 1", "Rex 2", "Rex 3"], 10, 10).await;
    let (names, _) = world.found(&caller, json!({ "name": "Rex" })).await;
    assert_eq!(names, ["Rex 3", "Rex 2", "Rex 1"]);

    // One blocked the caller: they are in nobody's way but not in this list.
    world
        .sql(format!(
            "INSERT INTO friend_blocks (blocker, blocked, id, name, at)
             VALUES ('{}', '{}', 'bl_x', 'Player 1', 1)",
            others[1].discord, caller.discord
        ))
        .await;
    let (names, _) = world.found(&caller, json!({ "name": "Rex" })).await;
    assert_eq!(names, ["Rex 3", "Rex 1"]);
    // The caller blocking someone does not hide them from the caller.
    world
        .sql(format!(
            "INSERT INTO friend_blocks (blocker, blocked, id, name, at)
             VALUES ('{}', '{}', 'bl_y', 'Rex 3', 1)",
            caller.discord, others[2].discord
        ))
        .await;
    let (names, _) = world.found(&caller, json!({ "name": "Rex" })).await;
    assert_eq!(names, ["Rex 3", "Rex 1"]);
    // The one who blocked still finds the player they blocked.
    let (names, _) = world.found(&others[1], json!({ "name": "Player 1" })).await;
    assert_eq!(names, ["Player 1"]);

    // A name whose Discord account is gone is not a player to find.
    world
        .sql(format!(
            "DELETE FROM discord_accounts WHERE user_id = '{}'",
            others[0].discord
        ))
        .await;
    let (names, _) = world.found(&caller, json!({ "name": "Rex" })).await;
    assert_eq!(names, ["Rex 3"]);
}

/// Turning friends off is how a player leaves search (docs/design/PROFILE.md,
/// "Migration 064"): by name as by code. Their card row stays, and is still
/// served from a room's token; leaving the ranking hides nobody.
#[tokio::test]
async fn a_player_with_friends_off_is_not_found_by_name_and_keeps_their_card() {
    let world = World::start().await;
    let caller = world.joined(1, 1, "Player 1").await;
    let rex = world.joined(2, 2, "Rex").await;
    world
        .save(&rex, json!({ "listing": { "name": "Rex" }, "main": 7 }))
        .await;
    let (names, _) = world.found(&caller, json!({ "name": "Rex" })).await;
    assert_eq!(names, ["Rex"]);

    let (status, _) = world
        .call(
            Method::POST,
            &rex,
            ACTIONS_PATH,
            Some(json!({ "action": "forget" })),
        )
        .await;
    assert_ne!(status, StatusCode::OK);
    let (names, more) = world.found(&caller, json!({ "name": "Rex" })).await;
    assert!(names.is_empty());
    assert!(!more);
    assert!(world.stored(&rex.discord).await.is_some());
    let card = world.card_of(&caller, &rex).await;
    assert_eq!(card["name"], "Rex");
    assert_eq!(card["main"], 7);

    // Friends on again: found again.
    world.act(&rex, json!({ "action": "turn_on" })).await;
    let (names, _) = world.found(&caller, json!({ "name": "Rex" })).await;
    assert_eq!(names, ["Rex"]);

    // Leaving the ranking hides no one: cards are public.
    world
        .sql(format!(
            "INSERT INTO ranking_choices (discord_user_id, ranked) VALUES ('{}', 0)",
            rex.discord
        ))
        .await;
    let (names, _) = world.found(&caller, json!({ "name": "Rex" })).await;
    assert_eq!(names, ["Rex"]);
}

#[tokio::test]
async fn a_search_lists_twenty_and_says_when_more_matched() {
    let world = World::start().await;
    let caller = world.member(1, 1, "Player 1").await;
    let mut statements = String::new();
    for n in 1..=22u64 {
        statements.push_str(&format!(
            "INSERT INTO identities (ember_id, public_key, created_at) VALUES ('emb_bulk_{n}', 'bulk-key-{n}', 1);
             INSERT INTO discord_accounts (user_id, ember_id, username, connected_at)
               VALUES ('{}', 'emb_bulk_{n}', 'bulk_{n}', 1);
             INSERT INTO player_cards (discord_user_id, name, since, updated_at)
               VALUES ('{}', 'Many {n:02}', 1, {});",
            account(1000 + n),
            account(1000 + n),
            1000 + n
        ));
    }
    // Twenty match: all are listed and no more are.
    world
        .sql(
            statements
                .replace("'Many 21'", "'Few 21'")
                .replace("'Many 22'", "'Few 22'"),
        )
        .await;
    let (names, more) = world.found(&caller, json!({ "name": "Many" })).await;
    assert_eq!(names.len(), MAX_SEARCH);
    assert!(!more);
    assert_eq!(
        (names[0].as_str(), names[19].as_str()),
        ("Many 20", "Many 01")
    );

    // Two more: the twenty newest, and a word that more matched.
    world
        .sql("UPDATE player_cards SET name = REPLACE(name, 'Few', 'Many')".into())
        .await;
    let (names, more) = world.found(&caller, json!({ "name": "Many" })).await;
    assert_eq!(names.len(), MAX_SEARCH);
    assert!(more);
    assert_eq!(
        (names[0].as_str(), names[19].as_str()),
        ("Many 22", "Many 03")
    );
}

#[tokio::test]
async fn a_name_is_two_to_thirty_two_bytes_and_one_of_name_or_code() {
    let world = World::start().await;
    let a = world.member(1, 1, "Player 1").await;
    for bad in [
        json!({ "name": "a" }),
        json!({ "name": "  a  " }),
        json!({ "name": "a".repeat(33) }),
        json!({ "name": "two\nlines" }),
        json!({ "code": "4F2A" }),
        json!({ "name": "Play", "code": "4F2A-91CX" }),
        json!({}),
        json!({ "name": "Play", "extra": 1 }),
    ] {
        let (status, answer) = world.try_search(&a, bad.clone()).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{bad}: {answer}");
    }
    world.search(&a, json!({ "name": "Pl" })).await;
    world.search(&a, json!({ "name": "a".repeat(32) })).await;
}

#[tokio::test]
async fn a_friend_code_finds_one_player_who_is_not_the_caller() {
    let world = World::start().await;
    let a = world.joined(1, 1, "Player 1").await;
    let b = world.joined(2, 2, "Player 2").await;
    let code = world.code_of(&b).await;
    let typed = code.to_lowercase().replace('-', " ").replace('0', "o");
    for text in [code.clone(), typed] {
        let answer = world.search(&a, json!({ "code": text })).await;
        assert_eq!(answer["players"].as_array().unwrap().len(), 1, "{answer}");
        assert_eq!(answer["players"][0]["name"], "Player 2");
        assert_eq!(answer["more"], false);
    }
    // Not yourself; not a code nobody holds.
    let (names, _) = world
        .found(&a, json!({ "code": world.code_of(&a).await }))
        .await;
    assert!(names.is_empty());
    let (names, _) = world.found(&a, json!({ "code": "ZZZZ-ZZZZ" })).await;
    assert!(names.is_empty());
    // Nor a player who blocked the caller, nor one with friends off, whose
    // code stopped working.
    world
        .sql(format!(
            "INSERT INTO friend_blocks (blocker, blocked, id, name, at) VALUES ('{}', '{}', 'bl_x', 'a', 1)",
            b.discord, a.discord
        ))
        .await;
    let (names, _) = world.found(&a, json!({ "code": code })).await;
    assert!(names.is_empty());
    world.sql("DELETE FROM friend_blocks".into()).await;
    let (names, _) = world.found(&a, json!({ "code": code })).await;
    assert_eq!(names, ["Player 2"]);
    // Turning friends off replaces the code: the old one finds nobody.
    let (status, _) = world
        .call(
            Method::POST,
            &b,
            ACTIONS_PATH,
            Some(json!({ "action": "forget" })),
        )
        .await;
    assert_ne!(status, StatusCode::OK);
    let (names, _) = world.found(&a, json!({ "code": code })).await;
    assert!(names.is_empty());
}

#[tokio::test]
async fn a_friend_code_is_no_selector_while_friends_are_off() {
    let world = World::start_with(false, false).await;
    let a = world.member(1, 1, "Player 1").await;
    let b = world.member(2, 2, "Player 2").await;
    world
        .save(&b, json!({ "listing": { "name": "Player 2" } }))
        .await;
    world
        .sql(format!(
            "INSERT INTO social_accounts (discord_user_id, name, code, code_at, seen_at)
             VALUES ('{}', 'Player 2', '4F2A-91CX', 1, 1)",
            b.discord
        ))
        .await;
    // Names are found; a code and a request name nobody.
    let (names, _) = world.found(&a, json!({ "name": "Player 2" })).await;
    assert_eq!(names, ["Player 2"]);
    let (names, _) = world.found(&a, json!({ "code": "4F2A-91CX" })).await;
    assert!(names.is_empty());
    let cards = world
        .cards(
            &a,
            json!([{ "code": "4F2A-91CX" },
                   { "request": "rq_11111111-1111-4111-8111-111111111111" }]),
        )
        .await;
    assert_eq!(cards, json!([null, null]));
}
