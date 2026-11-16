//! Friends (docs/design/FRIENDS.md): requests by card and by code, mutual
//! requests, requests that stay "sent" when the receiver never sees them, the
//! request limits, and the switch that offers friends. Blocks and turning
//! friends off are in `friends_blocks.rs`, presence, points, appearing
//! offline, codes and rooms in `friends_presence.rs`.
//!
//! Every answer a test here receives is kept, and scanned when its `World`
//! ends for the Discord IDs and names it seeded, and the friends and card
//! answers also for the Ember IDs: none may carry one
//! (`common::friends`). `every_action_and_route_is_scanned` sends every
//! action and calls every route that serves one player's data to another.
mod common;

use std::collections::BTreeSet;

use common::{
    friends::*,
    public_rooms::{player, reason},
    supervisor::{BUILD, ENDPOINT},
};
use ember_bridge::config;
use ember_protocol::{
    friends::{ACTIONS_PATH, SYNC_PATH},
    levels::{CARD_VALID_SECS, LOOKUP_PATH, POINTS_PATH},
    skill::{BOARD_PATH, STANDING_PATH},
};
use reqwest::StatusCode;
use serde_json::{Value as Json_, json};

#[tokio::test]
async fn friends_are_offered_beside_levels_where_discord_sign_in_is() {
    let world = World::start().await;
    assert!(offers(&world.bridge, "friends").await);
    assert!(offers(&world.bridge, "points").await);
    // A player without a Discord account has no friends list.
    let lone = player(&world.bridge, 9).await;
    let lone = Member {
        player: lone,
        discord: String::new(),
        name: "Player 9",
    };
    let body = json!({ "build_id": BUILD, "listing": { "name": "Player 9" }, "presence": menus() });
    let (status, answer) = world.post(&lone, SYNC_PATH, body.clone()).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{answer}");
    assert_eq!(reason(&answer), "no_discord_account");
    // An action before any sync has no state to answer.
    let first = world.member(1, A, "Player 1").await;
    let (status, answer) = world.try_act(&first, json!({ "action": "new_code" })).await;
    assert_eq!(status, StatusCode::CONFLICT, "{answer}");

    // Public rooms without Discord sign-in: no friends, and 404.
    let (rooms, _fake) = common::public_rooms::start().await;
    assert!(offers(&rooms, "points").await);
    assert!(!offers(&rooms, "friends").await);
    let caller = player(&rooms, 1).await;
    for path in [SYNC_PATH, ACTIONS_PATH] {
        let (status, _) = rooms.post(caller.token(), path, body.clone()).await;
        assert_eq!(status, StatusCode::NOT_FOUND, "{path}");
    }
}

#[tokio::test]
async fn a_request_by_card_or_by_code_is_sent_and_accepted() {
    let world = World::start().await;
    let (a, b, c) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
        world.join(3, C, "Player 3").await,
    );
    let sent = world.ask(&a, &b).await;
    assert_eq!(sent["outgoing"][0]["label"], "Player 2");
    let seen = world.sync(&b, menus()).await;
    let incoming = &seen["incoming"][0];
    assert_eq!(
        (incoming["name"].clone(), incoming["country"].clone()),
        (json!("Player 1"), json!("GB"))
    );
    // They never played: nothing of a record.
    assert!(incoming.get("played").is_none(), "{incoming}");
    assert_eq!(world.relation(&a, &b).await, "sent");
    assert_eq!(world.relation(&b, &a).await, "received");

    let request = incoming["id"].clone();
    let accepted = world
        .act(&b, json!({ "action": "accept", "request": request }))
        .await;
    assert!(ids(&accepted["incoming"]).is_empty());
    let friend = accepted["friends"][0]["id"].clone();
    let friends = world.sync(&a, menus()).await;
    assert_eq!(friends["friends"][0]["id"], friend);
    assert_eq!(friends["friends"][0]["name"], "Player 2");
    assert_eq!(friends["friends"][0]["main"], json!({ "fighter": 3 }));
    assert!(ids(&friends["outgoing"]).is_empty());
    assert_eq!(world.relation(&a, &b).await, "friend");

    // A friend's whole card by the friendship's id; nobody else's friend.
    let card = world.cards(&a, json!([{ "friend": friend }])).await;
    assert_eq!(card[0]["relation"], "friend");
    assert_eq!(
        world.cards(&c, json!([{ "friend": friend }])).await,
        json!([null])
    );

    // By code, as typed: any case, spaces, O for 0. The label is the code.
    let code = friends["code"].as_str().unwrap().to_owned();
    let typed = code.to_lowercase().replace('-', " ").replace('0', "o");
    let asked = world
        .act(&c, json!({ "action": "request", "code": typed }))
        .await;
    assert_eq!(asked["outgoing"][0]["label"], code);
    assert_eq!(
        world.sync(&a, menus()).await["incoming"][0]["name"],
        "Player 3"
    );

    let refusals = [
        (
            json!({ "action": "request", "code": "0000-0000" }),
            "unknown_code",
        ),
        (json!({ "action": "request", "code": code }), "own_card"),
        (
            json!({ "action": "request", "card": world.card(&a), "label": "Me" }),
            "own_card",
        ),
        (
            json!({ "action": "request", "label": "Player 2",
                "card": sealed(&world.bridge, B, world.now() - CARD_VALID_SECS - 1) }),
            "card_expired",
        ),
    ];
    for (action, expected) in refusals {
        assert_eq!(
            world.refused(&a, action.clone()).await,
            expected,
            "{action}"
        );
    }
}

#[tokio::test]
async fn asking_someone_who_asked_you_makes_you_friends() {
    let world = World::start().await;
    let (a, b) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
    );
    world.ask(&a, &b).await;
    let code = world.sync(&a, menus()).await["code"].clone();
    let both = world
        .act(&b, json!({ "action": "request", "code": code }))
        .await;
    assert_eq!(both["friends"][0]["name"], "Player 1");
    assert!(ids(&both["incoming"]).is_empty() && ids(&both["outgoing"]).is_empty());
    let other = world.sync(&a, menus()).await;
    assert_eq!(other["friends"][0]["id"], both["friends"][0]["id"]);
    assert!(ids(&other["outgoing"]).is_empty());
    // Asking a friend changes nothing.
    let again = world.ask(&a, &b).await;
    assert!(ids(&again["outgoing"]).is_empty());
}

#[tokio::test]
async fn a_declined_blocked_or_unseen_request_still_looks_sent() {
    let world = World::start().await;
    let (a, b, c) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
        world.join(3, C, "Player 3").await,
    );
    // Declined: gone for the receiver, sent for the sender.
    let sent = ids(&world.ask(&a, &b).await["outgoing"]);
    let request = world.sync(&b, menus()).await["incoming"][0]["id"].clone();
    let declined = world
        .act(&b, json!({ "action": "decline", "request": request }))
        .await;
    assert!(ids(&declined["incoming"]).is_empty());
    assert_eq!(ids(&world.sync(&a, menus()).await["outgoing"]), sent);
    assert_eq!(world.relation(&a, &b).await, "sent");
    assert_eq!(world.relation(&b, &a).await, "none");
    // Asking again changes nothing; taking it back and asking again shows it
    // sent once more, and the receiver still does not see it.
    assert_eq!(ids(&world.ask(&a, &b).await["outgoing"]), sent);
    let cancelled = world
        .act(&a, json!({ "action": "cancel", "request": sent[0] }))
        .await;
    assert!(ids(&cancelled["outgoing"]).is_empty());
    assert_eq!(world.relation(&a, &b).await, "none");
    assert_eq!(ids(&world.ask(&a, &b).await["outgoing"]).len(), 1);
    assert!(ids(&world.sync(&b, menus()).await["incoming"]).is_empty());

    // Blocked: the blocked player's request looks sent, and the blocker's
    // card is gone for them.
    let card = world.card(&c);
    world
        .act(
            &b,
            json!({ "action": "block", "card": card, "label": "Player 3" }),
        )
        .await;
    let code = world.sync(&b, menus()).await["code"].clone();
    let asked = world
        .act(&c, json!({ "action": "request", "code": code }))
        .await;
    assert_eq!(ids(&asked["outgoing"]).len(), 1);
    assert!(ids(&world.sync(&b, menus()).await["incoming"]).is_empty());
    assert_eq!(world.relation(&b, &c).await, "blocked");
    let blocker = world.card(&b);
    assert_eq!(
        world.cards(&c, json!([{ "card": blocker }])).await,
        json!([null])
    );

    // A request lapses 30 days after it was sent; then it may be sent again.
    world.bridge.clock.advance(30 * DAY);
    ember_bridge::maintain(world.bridge.state()).await;
    let mut a = a;
    world.renew(&mut a).await;
    assert!(ids(&world.sync(&a, menus()).await["outgoing"]).is_empty());
}

#[tokio::test]
async fn daily_pending_and_inbox_limits_hold() {
    let world = World::start().await;
    let (mut a, b, c) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
        world.join(3, C, "Player 3").await,
    );
    let now = world.now();
    let seeded = |n: u32| format!("8000000000000{n:05}");
    let request_id = |n: u32| format!("rq_00000000-0000-4000-8000-{n:012}");
    // 50 visible requests already wait for B: the next is stored hidden.
    let mut statement = String::new();
    for n in 0..50 {
        statement += &format!(
            "INSERT INTO social_accounts (discord_user_id, name, code, code_at, seen_at)
               VALUES ('{sender}', 'Player 9', 'S{n:07}', 0, 0);
             INSERT INTO friend_requests (id, sender, receiver, label, sent_at)
               VALUES ('{id}', '{sender}', '{B}', 'Player 2', {now});",
            sender = seeded(n),
            id = request_id(n),
        );
    }
    world.sql(statement).await;
    assert_eq!(ids(&world.ask(&c, &b).await["outgoing"]).len(), 1);
    let inbox = world.sync(&b, menus()).await;
    assert_eq!(ids(&inbox["incoming"]).len(), 50);
    assert!(
        inbox["incoming"]
            .as_array()
            .unwrap()
            .iter()
            .all(|r| r["name"] == "Player 9")
    );

    // 20 new requests a day.
    let mut statement = String::new();
    for n in 100..120 {
        statement += &format!(
            "INSERT INTO friend_requests (id, sender, receiver, label, sent_at)
               VALUES ('{}', '{A}', '{}', 'Player 9', {});",
            request_id(n),
            seeded(n),
            now - 60
        );
    }
    world.sql(statement).await;
    let card = world.card(&b);
    let ask_b = json!({ "action": "request", "card": card, "label": "Player 2" });
    assert_eq!(world.refused(&a, ask_b).await, "request_limit");
    world.bridge.clock.advance(DAY);
    world.renew(&mut a).await;
    assert_eq!(ids(&world.ask(&a, &b).await["outgoing"]).len(), 21);

    // 30 pending, hidden ones too.
    let mut statement = String::new();
    for n in 200..209 {
        statement += &format!(
            "INSERT INTO friend_requests (id, sender, receiver, label, sent_at, hidden_at)
               VALUES ('{}', '{A}', '{}', 'Player 9', {}, {});",
            request_id(n),
            seeded(n),
            now,
            now
        );
    }
    world.sql(statement).await;
    let card = world.card(&c);
    let ask_c = json!({ "action": "request", "card": card, "label": "Player 3" });
    assert_eq!(world.refused(&a, ask_c.clone()).await, "request_limit");
    world
        .act(
            &a,
            json!({ "action": "cancel", "request": request_id(100) }),
        )
        .await;
    assert_eq!(ids(&world.act(&a, ask_c).await["outgoing"]).len(), 30);
}

/// Every action, and every route that serves one player's data to another,
/// answers without a Discord ID or name; the friends and card answers also
/// without an Ember ID (`World`'s drop scans them all).
#[tokio::test]
async fn every_action_and_route_is_scanned() {
    let world = World::start().await;
    let (a, b, c) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
        world.join(3, C, "Player 3").await,
    );
    let friend = world.befriend(&a, &b).await;
    let room_id = world.room(&a, 2).await;
    world
        .sync(
            &b,
            json!({ "activity": "room",
            "room": { "room_id": room_id, "status": "watching", "table": 1 } }),
        )
        .await;
    let secret = format!("emd2:{}", "a-_9".repeat(30) + "ab");
    world
        .act(
            &a,
            json!({ "action": "invite", "friend": friend, "room": { "secret": secret } }),
        )
        .await;
    let invite = world.sync(&b, menus()).await["invites"][0]["id"].clone();
    world
        .act(&b, json!({ "action": "dismiss", "invite": invite }))
        .await;
    // A private room on offer: no answer shows its invitation but the one a
    // join gets (`World`'s drop scans them all).
    world.sync(&b, world.offering('x', 2, 8)).await;
    let seen = world.sync(&a, menus()).await;
    assert_eq!(
        seen["friends"][0]["presence"]["private_room"]["joinable"],
        true
    );
    let joined = world
        .act(&a, json!({ "action": "join", "friend": friend }))
        .await;
    assert_eq!(joined["joining"]["secret"], invitation('x'));
    assert!(world.sync(&a, menus()).await.get("joining").is_none());
    world.ask(&c, &a).await;
    let request = world.sync(&a, menus()).await["incoming"][0]["id"].clone();
    world
        .act(&a, json!({ "action": "decline", "request": request }))
        .await;
    let sent = world.sync(&c, menus()).await["outgoing"][0]["id"].clone();
    world
        .act(&c, json!({ "action": "cancel", "request": sent }))
        .await;
    let card = world.card(&c);
    let blocked = world
        .act(
            &b,
            json!({ "action": "block", "card": card, "label": "Player 3" }),
        )
        .await;
    world
        .act(
            &b,
            json!({ "action": "unblock", "block": blocked["blocked"][0]["id"] }),
        )
        .await;
    world
        .act(&a, json!({ "action": "appear_offline", "on": true }))
        .await;
    world.refused(&a, json!({ "action": "new_code" })).await;
    world
        .cards(
            &b,
            json!([{ "card": world.card(&a) }, { "friend": friend }]),
        )
        .await;
    world
        .act(&b, json!({ "action": "remove", "friend": friend }))
        .await;
    world.refused(&b, json!({ "action": "forget" })).await;
    world.act(&b, json!({ "action": "turn_on" })).await;
    for path in [POINTS_PATH, STANDING_PATH, BOARD_PATH, "/v1/rooms?detail=1"] {
        let (status, answer) = world.get(&a, path).await;
        assert_eq!(status, StatusCode::OK, "{path}: {answer}");
    }
    let ticket = format!("/v1/rooms/{room_id}/tickets");
    let (status, answer) = world
        .post(
            &b,
            &ticket,
            json!({ "endpoint_id": ENDPOINT, "build_id": BUILD, "card": true }),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{answer}");

    assert_eq!(
        *world.sent.lock().unwrap(),
        ACTIONS.into_iter().collect::<BTreeSet<_>>()
    );
    let paths: BTreeSet<String> = world
        .answers
        .lock()
        .unwrap()
        .iter()
        .map(|(path, _)| path.clone())
        .collect();
    for path in [
        SYNC_PATH,
        ACTIONS_PATH,
        LOOKUP_PATH,
        POINTS_PATH,
        STANDING_PATH,
        BOARD_PATH,
    ] {
        assert!(paths.contains(path), "{path} was not called");
    }
}

#[test]
fn friends_are_on_unless_the_rooms_setting_turns_them_off() {
    let rooms: config::Rooms =
        serde_json::from_value(json!({ "supervisor_url": "http://127.0.0.1:8790" })).unwrap();
    assert!(rooms.friends);
    let rooms: config::Rooms = serde_json::from_value(
        json!({ "supervisor_url": "http://127.0.0.1:8790", "friends": false }),
    )
    .unwrap();
    assert!(!rooms.friends);
}

/// An operator can switch friends off: with the switch off, Discord sign-in and public rooms
/// do not offer them, their routes answer as an unknown route, a look-up
/// answers null for a friend selector, and a card's relation is none.
#[tokio::test]
async fn friends_stay_dormant_while_the_switch_is_off() {
    let world = World::start_with(false).await;
    assert!(!offers(&world.bridge, "friends").await);
    // Points and the card look-up do not depend on the switch.
    assert!(offers(&world.bridge, "points").await);
    let a = world.member(1, A, "Player 1").await;
    let b = world.member(2, B, "Player 2").await;

    let sync = json!({ "build_id": BUILD, "listing": { "name": "Player 1" }, "presence": menus() });
    let (status, _) = world.post(&a, SYNC_PATH, sync).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = world
        .post(&a, ACTIONS_PATH, json!({ "action": "new_code" }))
        .await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let friend = "fr_11111111-1111-4111-8111-111111111111";
    let cards = world
        .cards(
            &a,
            json!([{ "friend": friend }, { "card": world.card(&b) }]),
        )
        .await;
    assert_eq!(cards[0], Json_::Null);
    assert_eq!(cards[1]["relation"], json!("none"));
}
