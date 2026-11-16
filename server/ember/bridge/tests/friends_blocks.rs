//! Friends (docs/design/FRIENDS.md): a block, which removes everything
//! between two players both ways and hides them from cards, and turning
//! friends off and on again. Every answer is scanned as `friends.rs` says.
mod common;

use common::{friends::*, public_rooms::reason, supervisor::BUILD};
use ember_protocol::friends::SYNC_PATH;
use reqwest::StatusCode;
use serde_json::{Value as Json_, json};

#[tokio::test]
async fn a_block_removes_everything_both_ways_and_hides_from_cards() {
    let world = World::start().await;
    let (a, b, c) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
        world.join(3, C, "Player 3").await,
    );
    let friend = world.befriend(&a, &b).await;
    world
        .act(
            &a,
            json!({ "action": "invite", "friend": friend,
            "room": { "room_id": "0f0123456789abcdef0123456789abcd" } }),
        )
        .await;
    assert_eq!(ids(&world.sync(&b, menus()).await["invites"]).len(), 1);

    let removed = world
        .act(
            &a,
            json!({ "action": "remove", "friend": friend, "block": true }),
        )
        .await;
    assert!(ids(&removed["friends"]).is_empty());
    assert_eq!(removed["blocked"][0]["name"], "Player 2");
    let blocked = world.sync(&b, menus()).await;
    assert!(ids(&blocked["friends"]).is_empty() && ids(&blocked["invites"]).is_empty());
    assert!(!blocked.to_string().contains("Player 1"), "{blocked}");
    assert_eq!(world.relation(&a, &b).await, "blocked");
    let blocker = world.card(&a);
    assert_eq!(
        world.cards(&b, json!([{ "card": blocker }])).await,
        json!([null])
    );
    // The blocked player's friend id no longer opens a card either.
    assert_eq!(
        world.cards(&b, json!([{ "friend": friend }])).await,
        json!([null])
    );

    // Decline and block lists the sender by their own name.
    world.ask(&c, &a).await;
    let request = world.sync(&a, menus()).await["incoming"][0]["id"].clone();
    let declined = world
        .act(
            &a,
            json!({ "action": "decline", "request": request, "block": true }),
        )
        .await;
    let names: Vec<&Json_> = declined["blocked"]
        .as_array()
        .unwrap()
        .iter()
        .map(|b| &b["name"])
        .collect();
    assert_eq!(names.len(), 2);
    assert!(names.contains(&&json!("Player 3")));

    // Unblocking lifts the block; what it hid stays hidden.
    let block = declined["blocked"]
        .as_array()
        .unwrap()
        .iter()
        .find(|entry| entry["name"] == "Player 3")
        .unwrap()["id"]
        .clone();
    let lifted = world
        .act(&a, json!({ "action": "unblock", "block": block }))
        .await;
    assert_eq!(ids(&lifted["blocked"]).len(), 1);
    assert!(ids(&lifted["incoming"]).is_empty());
    assert_eq!(world.relation(&c, &a).await, "sent");
}

/// Turning friends off ends everything friends have of the player, keeps what
/// protects them and is the answer to no friends route but `turn_on`.
#[tokio::test]
async fn turning_friends_off_deletes_the_friends_but_keeps_the_players_blocks() {
    let world = World::start().await;
    let (a, b, c, d) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
        world.join(3, C, "Player 3").await,
        world.join(4, D, "Player 4").await,
    );
    world.befriend(&a, &b).await;
    let card = world.card(&c);
    world
        .act(
            &a,
            json!({ "action": "block", "card": card, "label": "Player 3" }),
        )
        .await;
    world.ask(&d, &a).await;
    // C's own block on A is C's to keep.
    let card = world.card(&a);
    world
        .act(
            &c,
            json!({ "action": "block", "card": card, "label": "Player 1" }),
        )
        .await;
    let old = world.sync(&a, menus()).await["code"].clone();

    // The answer to the action itself is the refusal.
    assert_eq!(
        world.refused(&a, json!({ "action": "forget" })).await,
        "friends_off"
    );
    assert!(ids(&world.sync(&b, menus()).await["friends"]).is_empty());
    assert!(ids(&world.sync(&d, menus()).await["outgoing"]).is_empty());
    assert_eq!(
        world
            .refused(&b, json!({ "action": "request", "code": old }))
            .await,
        "unknown_code"
    );
    for (query, rows) in [
        (
            "SELECT COUNT(*) FROM friendships WHERE first = ?1 OR second = ?1",
            0,
        ),
        (
            "SELECT COUNT(*) FROM friend_requests WHERE sender = ?1 OR receiver = ?1",
            0,
        ),
        // The player's own blocks stay, and so does what others placed.
        ("SELECT COUNT(*) FROM friend_blocks WHERE blocker = ?1", 1),
        ("SELECT COUNT(*) FROM friend_blocks WHERE blocked = ?1", 1),
        (
            "SELECT COUNT(*) FROM social_accounts WHERE discord_user_id = ?1 AND off = 1",
            1,
        ),
    ] {
        assert_eq!(world.count(query, A).await, rows, "{query}");
    }
}

/// The game syncs by itself every 30 seconds and at launch: that must never
/// turn friends on again, and everything but `turn_on` says they are off.
#[tokio::test]
async fn an_account_that_is_off_refuses_every_route_but_turn_on() {
    let world = World::start().await;
    let a = world.join(1, A, "Player 1").await;
    world.refused(&a, json!({ "action": "forget" })).await;
    let sync = json!({ "build_id": BUILD, "listing": { "name": "Player 1", "country": "GB" },
        "main": 3, "presence": menus() });
    for _ in 0..2 {
        let (status, answer) = world.post(&a, SYNC_PATH, sync.clone()).await;
        assert_ne!(status, StatusCode::OK, "{answer}");
        assert_eq!(reason(&answer), "friends_off");
    }
    for action in [
        json!({ "action": "new_code" }),
        json!({ "action": "appear_offline", "on": true }),
        json!({ "action": "forget" }),
        json!({ "action": "request", "code": "0000-0000" }),
        json!({ "action": "block", "card": world.card(&a), "label": "Player 1" }),
    ] {
        assert_eq!(
            world.refused(&a, action.clone()).await,
            "friends_off",
            "{action}"
        );
    }
    assert_eq!(
        world
            .count(
                "SELECT off FROM social_accounts WHERE discord_user_id = ?1",
                A
            )
            .await,
        1
    );
}

/// Whoever asks an account that is off meets what they would meet at an
/// account that never synced: the code is unknown, and a card's request lands
/// where the sender cannot tell and the receiver never sees it.
#[tokio::test]
async fn nobody_can_reach_an_account_that_is_off() {
    let world = World::start().await;
    let (a, b, c) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
        world.join(3, C, "Player 3").await,
    );
    let friend = world.befriend(&a, &b).await;
    let secret = format!("emd2:{}", "a-_9".repeat(30) + "ab");
    world.refused(&a, json!({ "action": "forget" })).await;
    // By code: the one they hold now is not a player's either.
    assert!(ids(&world.sync(&b, menus()).await["friends"]).is_empty());
    let code = world.code_of(A).await;
    assert_eq!(
        world
            .refused(&c, json!({ "action": "request", "code": code }))
            .await,
        "unknown_code"
    );
    // By card: it looks sent, and nothing reaches them.
    let sent = world.ask(&c, &a).await;
    assert_eq!(ids(&sent["outgoing"]).len(), 1);
    assert_eq!(world.relation(&c, &a).await, "sent");
    // Nobody is invited to nothing: the friendship is gone.
    assert_eq!(
        world
            .refused(
                &b,
                json!({ "action": "invite", "friend": friend, "room": { "secret": secret } })
            )
            .await,
        "invite_unavailable"
    );
    // Turning friends on shows none of it.
    world.act(&a, json!({ "action": "turn_on" })).await;
    let state = world.sync(&a, menus()).await;
    assert!(ids(&state["incoming"]).is_empty(), "{state}");
    assert!(ids(&state["friends"]).is_empty());
    // And their new code works for whoever has it.
    let code = state["code"].clone();
    world
        .act(&b, json!({ "action": "request", "code": code }))
        .await;
    assert_eq!(ids(&world.sync(&a, menus()).await["incoming"]).len(), 1);
}

#[tokio::test]
async fn turning_friends_on_again_brings_back_the_account_and_its_blocks_but_not_the_old_code() {
    let world = World::start().await;
    let (a, b, c) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
        world.join(3, C, "Player 3").await,
    );
    let card = world.card(&c);
    let blocked = world
        .act(
            &a,
            json!({ "action": "block", "card": card, "label": "Player 3" }),
        )
        .await;
    let old = world.sync(&a, menus()).await["code"].clone();
    world.refused(&a, json!({ "action": "forget" })).await;

    let on = world.act(&a, json!({ "action": "turn_on" })).await;
    assert_ne!(on["code"], old);
    assert_eq!(on["blocked"], blocked["blocked"]);
    assert!(ids(&on["friends"]).is_empty());
    // The game's next sync is an ordinary one, and so are the others'.
    assert_eq!(world.sync(&a, menus()).await["code"], on["code"]);
    assert_eq!(
        world
            .refused(&b, json!({ "action": "request", "code": old }))
            .await,
        "unknown_code"
    );
    // The block still holds: C asks A and the request looks sent.
    world.ask(&c, &a).await;
    assert!(ids(&world.sync(&a, menus()).await["incoming"]).is_empty());
    // Turning on twice changes nothing.
    assert_eq!(
        world.act(&a, json!({ "action": "turn_on" })).await["code"],
        on["code"]
    );
    // A friend can be made again.
    world.befriend(&a, &b).await;
}
