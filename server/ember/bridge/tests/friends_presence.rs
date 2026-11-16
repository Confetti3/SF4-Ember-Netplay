//! Friends (docs/design/FRIENDS.md): a friend's public room and whether it is
//! joinable, the account points a friend carries, appearing offline, and a
//! new friend code. Every answer is scanned as `friends.rs` says.
mod common;

use common::{
    friends::*,
    supervisor::{BUILD, report},
};
use reqwest::StatusCode;
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
        json!({ "state": "room", "status": "fighting", "table": 0, "room": room,
            "game": "usf4" })
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
        json!({ "state": "room", "status": "idle", "game": "usf4" })
    );
    let private = json!({ "activity": "room",
        "room": { "private": true, "status": "training", "table": 3 } });
    world.sync(&b, private).await;
    assert_eq!(
        presence(world.sync(&a, menus()).await),
        json!({ "state": "room", "status": "training", "table": 3, "private": true,
            "game": "usf4" })
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
        json!({ "state": "online", "activity": "offline_play", "game": "usf4" })
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
        json!({ "state": "online", "activity": "menus", "game": "usf4" })
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
        json!({ "state": "online", "activity": "menus", "game": "usf4" })
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

fn private_room_without_offer() -> Json_ {
    json!({ "activity": "room", "room": { "private": true, "status": "idle" } })
}

/// A private room a friend offers shows its name, members and capacity and
/// whether this caller can join it, and never the invitation, which only a
/// join hands out. Appearing offline takes it back.
#[tokio::test]
async fn a_private_room_on_offer_shows_its_facts_and_never_its_invitation() {
    let world = World::start().await;
    let (a, b) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
    );
    world.befriend(&a, &b).await;
    let presence = |state: Json_| state["friends"][0]["presence"].clone();
    let lounge = |members: u64, joinable: bool| {
        json!({ "state": "room", "status": "idle", "private": true, "game": "usf4",
            "private_room": { "name": "Private lounge", "members": members,
                              "capacity": 8, "joinable": joinable } })
    };

    world.sync(&b, world.offering('x', 2, 8)).await;
    let state = world.sync(&a, menus()).await;
    assert_eq!(presence(state.clone()), lounge(2, true));
    assert!(!state.to_string().contains(&invitation('x')), "{state}");
    // Another build sees the room and cannot join it.
    assert_eq!(
        presence(world.sync_on(&a, "build-2", menus()).await),
        lounge(2, false)
    );
    // A full room is not joinable either.
    world.sync(&b, world.offering('x', 8, 8)).await;
    assert_eq!(presence(world.sync(&a, menus()).await), lounge(8, false));
    // Without an offer the room is "private" and no more.
    world.sync(&b, private_room_without_offer()).await;
    assert_eq!(
        presence(world.sync(&a, menus()).await),
        json!({ "state": "room", "status": "idle", "private": true, "game": "usf4" })
    );
    // Appearing offline hides the offer with the rest.
    world.sync(&b, world.offering('x', 2, 8)).await;
    assert_eq!(
        presence(world.sync(&a, menus()).await)["private_room"]["name"],
        "Private lounge"
    );
    world
        .act(&b, json!({ "action": "appear_offline", "on": true }))
        .await;
    let hidden = presence(world.sync(&a, menus()).await);
    assert_eq!(hidden["state"], "offline");
    assert!(hidden.get("private_room").is_none(), "{hidden}");
}

/// A friend in another game shows their game, what they are doing and the
/// table, and no room, public or private: rooms are shown only within one
/// game, whichever the game is.
#[tokio::test]
async fn a_friend_in_another_game_shows_the_game_and_no_room() {
    let world = World::start().await;
    let (a, b) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
    );
    world.befriend(&a, &b).await;
    let room_id = world.room(&a, 2).await;
    let presence = |state: Json_| state["friends"][0]["presence"].clone();
    let fighting = json!({ "activity": "room",
        "room": { "room_id": room_id, "status": "fighting", "table": 0 } });

    // A caller who names no game is in the default game, and so is a friend.
    world.sync(&b, fighting.clone()).await;
    assert_eq!(
        presence(world.sync(&a, menus()).await)["room"]["joinable"],
        true
    );

    // The friend is in another game: no public room, no private one.
    world.sync_in(&b, BUILD, "sfv", fighting.clone()).await;
    let other = json!({ "state": "room", "status": "fighting", "table": 0, "game": "sfv" });
    assert_eq!(presence(world.sync(&a, menus()).await), other);
    world
        .sync_in(&b, BUILD, "sfv", world.offering('x', 2, 8))
        .await;
    let state = world.sync(&a, menus()).await;
    assert_eq!(
        presence(state.clone()),
        json!({ "state": "room", "status": "idle", "private": true, "game": "sfv" })
    );
    assert!(!state.to_string().contains(&invitation('x')), "{state}");
    world.sync_in(&b, BUILD, "sfv", menus()).await;
    assert_eq!(
        presence(world.sync(&a, menus()).await),
        json!({ "state": "online", "activity": "menus", "game": "sfv" })
    );

    // Both in that game: a private room is shown, and a public room of
    // another game is not, though its id is a listed room's.
    world
        .sync_in(&b, BUILD, "sfv", world.offering('x', 2, 8))
        .await;
    let seen = world.sync_in(&a, BUILD, "sfv", menus()).await;
    assert_eq!(presence(seen)["private_room"]["joinable"], true);
    world.sync_in(&b, BUILD, "sfv", fighting.clone()).await;
    assert_eq!(
        presence(world.sync_in(&a, BUILD, "sfv", menus()).await),
        other
    );
    // The caller is in the other game, the friend in the default one.
    world.sync(&b, fighting).await;
    assert_eq!(
        presence(world.sync_in(&a, BUILD, "sfv", menus()).await),
        json!({ "state": "room", "status": "fighting", "table": 0, "game": "usf4" })
    );
}

/// A join hands out the invitation once, in its own answer, and a later
/// state carries none.
#[tokio::test]
async fn a_join_hands_out_the_invitation_once() {
    let world = World::start().await;
    let (a, b) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
    );
    let friend = world.befriend(&a, &b).await;
    world.sync(&b, world.offering('x', 2, 8)).await;
    let join = json!({ "action": "join", "friend": friend });

    let joined = world.act(&a, join.clone()).await;
    assert_eq!(
        joined["joining"],
        json!({ "friend": friend, "secret": invitation('x') })
    );
    // The rest of the answer is the state as a sync shows it.
    assert_eq!(
        joined["friends"][0]["presence"]["private_room"]["joinable"],
        true
    );
    assert!(joined["friends"][0]["presence"].get("secret").is_none());
    let later = world.sync(&a, menus()).await;
    assert!(later.get("joining").is_none(), "{later}");
    // The friend never sees their own invitation come back either.
    let own = world.sync(&b, menus()).await;
    assert!(!own.to_string().contains(&invitation('x')), "{own}");
}

/// Whatever is wrong, a join is refused the same way, so the caller learns
/// nothing of why: no offer, a full room, another build or game, a friend
/// who appears offline or went quiet.
#[tokio::test]
async fn a_join_is_refused_alike_for_every_reason() {
    let world = World::start().await;
    let (a, b) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
    );
    let friend = world.befriend(&a, &b).await;
    let join = json!({ "action": "join", "friend": friend });

    world.sync(&b, world.offering('x', 2, 8)).await;
    assert_eq!(
        world.act(&a, join.clone()).await["joining"]["secret"],
        invitation('x')
    );

    // Not in a room, or in a private one that offers nothing.
    world.sync(&b, menus()).await;
    assert_eq!(world.refused(&a, join.clone()).await, "join_unavailable");
    world.sync(&b, private_room_without_offer()).await;
    assert_eq!(world.refused(&a, join.clone()).await, "join_unavailable");
    // A full room.
    world.sync(&b, world.offering('x', 8, 8)).await;
    assert_eq!(world.refused(&a, join.clone()).await, "join_unavailable");
    world.sync(&b, world.offering('x', 2, 8)).await;
    // Another build.
    world.sync_on(&a, "build-2", menus()).await;
    assert_eq!(world.refused(&a, join.clone()).await, "join_unavailable");
    // Another game, whichever side is in it.
    world.sync_in(&a, BUILD, "sfv", menus()).await;
    assert_eq!(world.refused(&a, join.clone()).await, "join_unavailable");
    world.sync(&a, menus()).await;
    world
        .sync_in(&b, BUILD, "sfv", world.offering('x', 2, 8))
        .await;
    assert_eq!(world.refused(&a, join.clone()).await, "join_unavailable");
    world.sync(&b, world.offering('x', 2, 8)).await;
    // A friend who appears offline.
    world
        .act(&b, json!({ "action": "appear_offline", "on": true }))
        .await;
    assert_eq!(world.refused(&a, join.clone()).await, "join_unavailable");
    world
        .act(&b, json!({ "action": "appear_offline", "on": false }))
        .await;
    // A friend not heard from for more than 75 seconds.
    world.bridge.clock.advance(76);
    assert_eq!(world.refused(&a, join.clone()).await, "join_unavailable");
    // Back in the room, the join works again.
    world.sync(&b, world.offering('x', 2, 8)).await;
    world.sync(&a, menus()).await;
    assert_eq!(
        world.act(&a, join.clone()).await["joining"]["friend"],
        friend
    );
}

/// An ended friendship, a block, an id that is not the caller's friend's and
/// friends turned off all refuse a join.
#[tokio::test]
async fn a_join_needs_a_friend_and_friends_on() {
    let world = World::start().await;
    let (a, b, c) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
        world.join(3, C, "Player 3").await,
    );
    let with_b = world.befriend(&a, &b).await;
    let with_c = world.befriend(&a, &c).await;
    world.sync(&b, world.offering('x', 2, 8)).await;
    world.sync(&c, world.offering('y', 2, 8)).await;
    let join = |friend: &str| json!({ "action": "join", "friend": friend });
    assert_eq!(
        world.act(&a, join(&with_b)).await["joining"]["secret"],
        invitation('x')
    );
    assert_eq!(
        world.act(&a, join(&with_c)).await["joining"]["secret"],
        invitation('y')
    );

    world
        .act(&a, json!({ "action": "remove", "friend": with_b }))
        .await;
    assert_eq!(world.refused(&a, join(&with_b)).await, "join_unavailable");
    world
        .act(
            &a,
            json!({ "action": "remove", "friend": with_c, "block": true }),
        )
        .await;
    assert_eq!(world.refused(&a, join(&with_c)).await, "join_unavailable");
    // Another player's friendship is not the caller's to join by.
    assert_eq!(world.refused(&b, join(&with_c)).await, "join_unavailable");

    // Turned off, a join is refused as every route is.
    assert_eq!(
        world.refused(&a, json!({ "action": "forget" })).await,
        "friends_off"
    );
    assert_eq!(world.refused(&a, join(&with_b)).await, "friends_off");
}

/// A join is one of ten in ten minutes: each hands out an invitation.
#[tokio::test]
async fn joins_are_limited_to_ten_in_ten_minutes() {
    let world = World::start().await;
    let (a, b) = (
        world.join(1, A, "Player 1").await,
        world.join(2, B, "Player 2").await,
    );
    let friend = world.befriend(&a, &b).await;
    world.sync(&b, world.offering('x', 2, 8)).await;
    let join = json!({ "action": "join", "friend": friend });
    for _ in 0..10 {
        world.act(&a, join.clone()).await;
    }
    let (status, answer) = world.try_act(&a, join).await;
    assert_eq!(status, StatusCode::TOO_MANY_REQUESTS, "{answer}");
    assert!(!answer.to_string().contains(&invitation('x')), "{answer}");
}
