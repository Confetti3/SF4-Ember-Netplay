//! Friends (docs/design/FRIENDS.md): a friend's public room and whether it is
//! joinable, the account points a friend carries, appearing offline, and a
//! new friend code. Every answer is scanned as `friends.rs` says.
mod common;

use common::{friends::*, supervisor::report};
use serde_json::{Value as Json_, json};

#[tokio::test]
async fn a_friends_public_room_is_joinable_on_the_same_build() {
    let world = World::start().await;
    let (a, b) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
    );
    world.befriend(&a, &b).await;
    let room_id = world.room(&a, 2).await;
    let fighting = json!({ "activity": "room",
        "room": { "room_id": room_id, "status": "fighting", "table": 0 } });
    world.sync(&b, fighting.clone()).await;
    let presence = |state: Json_| state["friends"][0]["presence"].clone();
    let room = json!({ "room_id": room_id, "name": "Friendly matches",
        "members": 2, "capacity": 8, "joinable": true });
    assert_eq!(
        presence(world.sync(&a, menus()).await),
        json!({ "state": "room", "status": "fighting", "table": 0, "room": room })
    );
    // Another build sees the room, and cannot join it.
    let other = presence(world.sync_on(&a, "build-2", menus()).await);
    assert_eq!(other["room"]["joinable"], false);
    // A full room is not joinable either.
    report(&world.fake, &room_id, 8, 1, &[]);
    ember_bridge::poll_rooms(world.bridge.state()).await;
    assert_eq!(
        presence(world.sync(&a, menus()).await)["room"]["joinable"],
        false
    );

    // A room that is not listed is "in a room" and no more; a private one is
    // only private.
    let unlisted = json!({ "activity": "room",
        "room": { "room_id": "0f0123456789abcdef0123456789abcd", "status": "idle" } });
    world.sync(&b, unlisted).await;
    assert_eq!(
        presence(world.sync(&a, menus()).await),
        json!({ "state": "room", "status": "idle" })
    );
    let private = json!({ "activity": "room",
        "room": { "private": true, "status": "training", "table": 3 } });
    world.sync(&b, private).await;
    assert_eq!(
        presence(world.sync(&a, menus()).await),
        json!({ "state": "room", "status": "training", "table": 3, "private": true })
    );

    // Leaving: offline at once, seen as of now.
    world.sync(&b, json!({ "activity": "offline" })).await;
    assert_eq!(
        presence(world.sync(&a, menus()).await),
        json!({ "state": "offline", "seen_at": world.now() })
    );
    // A sync more than 75 seconds old is offline too.
    world.sync(&b, json!({ "activity": "offline_play" })).await;
    assert_eq!(
        presence(world.sync(&a, menus()).await),
        json!({ "state": "online", "activity": "offline_play" })
    );
    world.bridge.clock.advance(76);
    assert_eq!(presence(world.sync(&a, menus()).await)["state"], "offline");
}

/// A friend's account points come with them, read from the table the card
/// look-up reads: 0 before a counted game, games plus wins after, and the same
/// answer as the friend's card.
#[tokio::test]
async fn a_friend_carries_their_account_points() {
    let world = World::start().await;
    let (a, b) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
    );
    let friend = world.befriend(&a, &b).await;
    let account = |state: &Json_| state["friends"][0]["account"].clone();
    assert_eq!(
        account(&world.sync(&a, menus()).await),
        json!({ "points": 0, "tier": 0 })
    );
    world
        .sql(format!(
            "INSERT INTO account_points (discord_user_id, games, wins, first_at, played_at)
             VALUES ('{B}', 8, 5, 1, 1)"
        ))
        .await;
    let state = world.sync(&a, menus()).await;
    assert_eq!(account(&state)["points"], 13);
    let card = world.cards(&a, json!([{ "friend": friend }])).await;
    assert_eq!(account(&state), card[0]["account"]);
    // Only the points and the frame, never the games or the wins.
    assert_eq!(account(&state).as_object().unwrap().len(), 2);
}

#[tokio::test]
async fn appearing_offline_freezes_what_friends_see() {
    let world = World::start().await;
    let (mut a, mut b, mut c) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
        world.join(3, C, "Player 3").await,
    );
    let friend = world.befriend(&a, &b).await;
    let presence = |state: Json_| state["friends"][0]["presence"].clone();
    assert_eq!(
        presence(world.sync(&a, menus()).await),
        json!({ "state": "online", "activity": "menus" })
    );
    world.bridge.clock.advance(10);
    let hidden_at = world.now();
    let own = world
        .act(&b, json!({ "action": "appear_offline", "on": true }))
        .await;
    assert_eq!(own["appear_offline"], true);
    let offline = json!({ "state": "offline", "seen_at": hidden_at });
    assert_eq!(presence(world.sync(&a, menus()).await), offline);
    // B keeps syncing; friends keep seeing the moment it was turned on.
    for _ in 0..2 {
        world.bridge.clock.advance(400);
        world.renew(&mut a).await;
        world.renew(&mut b).await;
        world.renew(&mut c).await;
        world.sync(&b, menus()).await;
        assert_eq!(presence(world.sync(&a, menus()).await), offline);
    }
    // Nobody can invite a friend who appears offline; they can invite.
    let a_friend = world.sync(&b, menus()).await["friends"][0]["id"].clone();
    let room = json!({ "room_id": "0f0123456789abcdef0123456789abcd" });
    assert_eq!(
        world
            .refused(
                &a,
                json!({ "action": "invite", "friend": friend, "room": room })
            )
            .await,
        "invite_unavailable"
    );
    let invited = world
        .act(
            &b,
            json!({ "action": "invite", "friend": a_friend, "room": room }),
        )
        .await;
    assert!(ids(&invited["invites"]).is_empty());
    let invites = world.sync(&a, menus()).await["invites"].clone();
    assert_eq!(invites[0]["friend"], friend);
    assert_eq!(invites[0]["room"], room);
    // Not a friend: nobody to invite.
    assert_eq!(
        world
            .refused(
                &c,
                json!({ "action": "invite", "friend": friend, "room": room })
            )
            .await,
        "invite_unavailable"
    );
    let dismissed = world
        .act(
            &a,
            json!({ "action": "dismiss", "invite": invites[0]["id"] }),
        )
        .await;
    assert!(ids(&dismissed["invites"]).is_empty());

    world
        .act(&b, json!({ "action": "appear_offline", "on": false }))
        .await;
    assert_eq!(
        presence(world.sync(&a, menus()).await),
        json!({ "state": "online", "activity": "menus" })
    );
    // Visible again, B leaving is seen as of now.
    world.sync(&b, menus()).await;
    world.sync(&b, json!({ "activity": "offline" })).await;
    assert_eq!(
        presence(world.sync(&a, menus()).await),
        json!({ "state": "offline", "seen_at": world.now() })
    );
}

#[tokio::test]
async fn a_new_code_once_a_day_and_the_old_one_stops_working() {
    let world = World::start().await;
    let (mut a, mut b) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
    );
    let old = world.sync(&a, menus()).await["code"].clone();
    assert_eq!(
        world.refused(&a, json!({ "action": "new_code" })).await,
        "code_too_soon"
    );
    world.bridge.clock.advance(DAY);
    world.renew(&mut a).await;
    world.renew(&mut b).await;
    let new = world.act(&a, json!({ "action": "new_code" })).await["code"].clone();
    assert_ne!(new, old);
    assert_eq!(
        world
            .refused(&b, json!({ "action": "request", "code": old }))
            .await,
        "unknown_code"
    );
    world
        .act(&b, json!({ "action": "request", "code": new }))
        .await;
    assert_eq!(
        world.sync(&a, menus()).await["incoming"][0]["name"],
        "Player 2"
    );
}
