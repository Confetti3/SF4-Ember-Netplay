//! The friend code on a card (docs/design/PROFILE.md, "The friend code on a
//! card"): a bridge whose friends are off and whose game is not USF4 keeps
//! the code each player saves with their card and shows it on that card to
//! whoever looks it up. Any other bridge does not offer it, refuses a save
//! that names one and shows none.

mod common;

use common::profiles::*;
use ember_protocol::{friends::FRIENDS_FEATURE, levels::CARD_FRIEND_CODE_FEATURE};
use reqwest::StatusCode;
use serde_json::{Value as Json_, json};

const CODE: &str = "4F2A-91CX";

fn card_with_code() -> Json_ {
    json!({ "listing": { "name": "Player 2", "country": "FR" }, "main": 5, "friend_code": CODE })
}

#[tokio::test]
async fn a_bridge_whose_friends_are_elsewhere_shows_the_code_a_player_saved() {
    let world = World::start_for(false, false, "sfv").await;
    assert!(offers(&world.bridge, CARD_FRIEND_CODE_FEATURE).await);
    assert!(!offers(&world.bridge, FRIENDS_FEATURE).await);
    let a = world.member(1, 1, "Player 1").await;
    let b = world.member(2, 2, "Player 2").await;
    world.save(&b, card_with_code()).await;

    // Another player holding a room member's token sees it, beside the rest
    // of the card; so does its own player.
    let card = world.card_of(&a, &b).await;
    assert_eq!(card["friend_code"], CODE);
    assert_eq!(card["name"], "Player 2");
    assert_eq!(card["main"], 5);
    assert_eq!(card["relation"], "none");
    assert_eq!(world.card_of(&b, &b).await["friend_code"], CODE);
    // A player who saved none shows none.
    world
        .save(&a, json!({ "listing": { "name": "Player 1" } }))
        .await;
    assert!(world.card_of(&b, &a).await.get("friend_code").is_none());

    // A search hit never carries it: the card it opens does.
    let answer = world.search(&a, json!({ "name": "Player 2" })).await;
    let hits = answer["players"].as_array().unwrap();
    assert_eq!(hits.len(), 1);
    assert!(hits[0].get("friend_code").is_none());
    let opened = world
        .cards(&a, json!([{ "card": hits[0]["card"].clone() }]))
        .await;
    assert_eq!(opened[0]["friend_code"], CODE);

    // A save without it clears it: the whole card is replaced.
    world.tick();
    world
        .save(&b, json!({ "listing": { "name": "Player 2" } }))
        .await;
    assert!(world.card_of(&a, &b).await.get("friend_code").is_none());

    // Only the form the bridge writes is saved; a refused save changes nothing.
    world.tick();
    world.save(&b, card_with_code()).await;
    for typed in ["4f2a-91cx", "4F2A 91CX", "4F2A-91C", ""] {
        let mut card = card_with_code();
        card["friend_code"] = json!(typed);
        let (status, answer) = world.try_save(&b, card).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{typed}: {answer}");
        assert_eq!(answer["error"]["code"], "invalid_request", "{typed}");
    }
    assert_eq!(world.card_of(&a, &b).await["friend_code"], CODE);
}

/// A bridge that does not show codes: friends on (as by default), or friends
/// off for USF4, whose players keep their friends on that same bridge.
async fn shows_no_code(world: World) {
    assert!(!offers(&world.bridge, CARD_FRIEND_CODE_FEATURE).await);
    let a = world.member(1, 1, "Player 1").await;
    let b = world.member(2, 2, "Player 2").await;
    // A save that names a code is a client's mistake here, and stores nothing.
    let (status, answer) = world.try_save(&b, card_with_code()).await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{answer}");
    assert_eq!(answer["error"]["code"], "invalid_request");
    assert!(world.stored(&b.discord).await.is_none());
    // A code put in the row by hand is still never shown.
    world
        .save(&b, json!({ "listing": { "name": "Player 2" } }))
        .await;
    world
        .sql(format!(
            "UPDATE player_cards SET friend_code = '{CODE}' WHERE discord_user_id = '{}'",
            b.discord
        ))
        .await;
    let card = world.card_of(&a, &b).await;
    assert_eq!(card["name"], "Player 2");
    assert!(card.get("friend_code").is_none());
}

#[tokio::test]
async fn a_bridge_with_friends_on_shows_no_code() {
    let world = World::start().await;
    assert!(offers(&world.bridge, FRIENDS_FEATURE).await);
    shows_no_code(world).await;
}

#[tokio::test]
async fn a_usf4_bridge_with_friends_off_shows_no_code() {
    let world = World::start_with(false, false).await;
    assert!(!offers(&world.bridge, FRIENDS_FEATURE).await);
    shows_no_code(world).await;
}
