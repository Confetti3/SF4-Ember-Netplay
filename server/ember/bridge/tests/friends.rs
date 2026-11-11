//! Friends (docs/design/FRIENDS.md): requests by card and by code, mutual
//! requests, requests that stay "sent" when the receiver never sees them, the
//! request limits, blocks, turning friends off, presence in public rooms,
//! appearing offline and invites.
//!
//! Every answer a test here receives is kept, and scanned when its `World`
//! ends for the Discord IDs and names it seeded, and the friends and card
//! answers also for the Ember IDs: none may carry one.
//! `every_action_and_route_is_scanned` sends every action and calls every
//! route that serves one player's data to another.
mod common;

use std::{collections::BTreeSet, sync::Mutex};

use common::{
    Bridge, Player,
    public_rooms::{open_room, player, reason},
    seed,
    supervisor::{self, BUILD, ENDPOINT, Supervisor, report},
};
use ember_bridge::{config, integrations::Secrets};
use ember_protocol::{
    encoding::b64u,
    friends::{ACTIONS_PATH, FriendAction, FriendsState, SYNC_PATH},
    levels::{CARD_PREFIX, CARD_VALID_SECS, POINTS_PATH, LOOKUP_PATH},
    skill::{BOARD_PATH, STANDING_PATH},
};
use reqwest::StatusCode;
use serde_json::{Value as Json_, json};
use zeroize::Zeroizing;

const DAY: i64 = 24 * 60 * 60;

/// Each action's name, by a match without a wildcard: a new action does not
/// build until it is named here, and `every_action_and_route_is_scanned`
/// must then send it (`ACTIONS`).
fn named(action: &FriendAction) -> &'static str {
    match action {
        FriendAction::Request { .. } => "request",
        FriendAction::Accept { .. } => "accept",
        FriendAction::Decline { .. } => "decline",
        FriendAction::Cancel { .. } => "cancel",
        FriendAction::Remove { .. } => "remove",
        FriendAction::Block { .. } => "block",
        FriendAction::Unblock { .. } => "unblock",
        FriendAction::Invite { .. } => "invite",
        FriendAction::Dismiss { .. } => "dismiss",
        FriendAction::AppearOffline { .. } => "appear_offline",
        FriendAction::NewCode {} => "new_code",
        FriendAction::Forget {} => "forget",
    }
}

const ACTIONS: [&str; 12] = [
    "request",
    "accept",
    "decline",
    "cancel",
    "remove",
    "block",
    "unblock",
    "invite",
    "dismiss",
    "appear_offline",
    "new_code",
    "forget",
];

/// A player with a Discord account.
struct Member {
    player: Player,
    discord: String,
    name: &'static str,
}

/// A bridge with public rooms and Discord sign-in, whose clock moves only
/// when a test moves it, and every answer it gave.
struct World {
    bridge: Bridge,
    fake: Supervisor,
    /// (path, answer) of every request a test made.
    answers: Mutex<Vec<(String, String)>>,
    /// The Discord IDs and names seeded: in no answer at all.
    accounts: Mutex<Vec<String>>,
    /// The Ember IDs seeded: in no friends or card answer.
    ember_ids: Mutex<Vec<String>>,
    /// The actions sent, by name.
    sent: Mutex<BTreeSet<&'static str>>,
}

impl Drop for World {
    fn drop(&mut self) {
        if std::thread::panicking() {
            return;
        }
        let accounts = self.accounts.lock().unwrap();
        let ember_ids = self.ember_ids.lock().unwrap();
        for (path, answer) in self.answers.lock().unwrap().iter() {
            for secret in accounts.iter() {
                assert!(!answer.contains(secret.as_str()), "{path} names {secret}: {answer}");
            }
            if path.starts_with("/v1/friends") || path == LOOKUP_PATH {
                for ember_id in ember_ids.iter() {
                    assert!(!answer.contains(ember_id.as_str()), "{path} names {ember_id}: {answer}");
                }
            }
        }
    }
}

/// Seals a card token for `discord` issued at `at`, as `cards::issue` does.
fn sealed(bridge: &Bridge, discord: &str, at: u64) -> String {
    let mut plain = vec![0x01];
    plain.extend(discord.parse::<u64>().unwrap().to_le_bytes());
    plain.extend(at.to_le_bytes());
    format!("{CARD_PREFIX}{}", b64u(&bridge.state().keys.seal(&plain)))
}

fn menus() -> Json_ {
    json!({ "activity": "menus" })
}

/// The ids of a list in a state.
fn ids(list: &Json_) -> Vec<String> {
    list.as_array()
        .unwrap()
        .iter()
        .map(|entry| entry["id"].as_str().unwrap().to_owned())
        .collect()
}

impl World {
    async fn start() -> Self {
        let (fake, url) = supervisor::fake_supervisor().await;
        let bridge = Bridge::start_with(
            |config| {
                supervisor::enable(config, url);
                config.discord = Some(config::Discord {
                    client_id: "1546980049692135514".into(),
                    api_base: "http://127.0.0.1:9/api".into(),
                });
            },
            Secrets {
                discord_client_secret: Some(Zeroizing::new("test-secret".into())),
                ..supervisor::secrets()
            },
        )
        .await;
        bridge.clock.freeze();
        Self {
            bridge,
            fake,
            answers: Mutex::default(),
            accounts: Mutex::default(),
            ember_ids: Mutex::default(),
            sent: Mutex::default(),
        }
    }

    fn now(&self) -> u64 {
        self.bridge.state().now()
    }

    /// A player with Discord account `discord`, who has synced once.
    async fn join(&self, byte: u8, discord: &str, name: &'static str) -> Member {
        let member = self.member(byte, discord, name).await;
        self.sync(&member, menus()).await;
        member
    }

    /// A player with Discord account `discord`, who never synced.
    async fn member(&self, byte: u8, discord: &str, name: &'static str) -> Member {
        let player = player(&self.bridge, byte).await;
        let username = format!("discord_name_{discord}");
        seed::link_discord(self.bridge.state(), discord, player.id().as_str(), &username).await;
        self.accounts
            .lock()
            .unwrap()
            .extend([discord.to_owned(), username]);
        self.ember_ids
            .lock()
            .unwrap()
            .push(player.id().as_str().to_owned());
        Member {
            player,
            discord: discord.to_owned(),
            name,
        }
    }

    /// A new session, for a member whose last one lapsed as the clock moved.
    async fn renew(&self, member: &mut Member) {
        self.bridge.open_session(&mut member.player).await;
    }

    fn record(&self, path: &str, answer: &Json_) {
        self.answers
            .lock()
            .unwrap()
            .push((path.to_owned(), answer.to_string()));
    }

    async fn post(&self, member: &Member, path: &str, body: Json_) -> (StatusCode, Json_) {
        let (status, answer) = self.bridge.post(member.player.token(), path, body).await;
        self.record(path, &answer);
        (status, answer)
    }

    async fn get(&self, member: &Member, path: &str) -> (StatusCode, Json_) {
        let (status, answer) = self.bridge.get(member.player.token(), path).await;
        self.record(path, &answer);
        (status, answer)
    }

    /// The state, checked as a client reads it.
    fn state_of(status: StatusCode, answer: Json_) -> Json_ {
        assert_eq!(status, StatusCode::OK, "{answer}");
        let state: FriendsState = serde_json::from_value(answer.clone()).unwrap();
        state.check().unwrap();
        answer
    }

    async fn sync_on(&self, member: &Member, build: &str, presence: Json_) -> Json_ {
        let body = json!({ "build_id": build, "listing": { "name": member.name, "country": "GB" },
            "main": 3, "presence": presence });
        let (status, answer) = self.post(member, SYNC_PATH, body).await;
        Self::state_of(status, answer)
    }

    async fn sync(&self, member: &Member, presence: Json_) -> Json_ {
        self.sync_on(member, BUILD, presence).await
    }

    async fn try_act(&self, member: &Member, action: Json_) -> (StatusCode, Json_) {
        let parsed: FriendAction = serde_json::from_value(action.clone()).unwrap();
        self.sent.lock().unwrap().insert(named(&parsed));
        self.post(member, ACTIONS_PATH, action).await
    }

    async fn act(&self, member: &Member, action: Json_) -> Json_ {
        let (status, answer) = self.try_act(member, action).await;
        Self::state_of(status, answer)
    }

    /// The refusal's reason for an action.
    async fn refused(&self, member: &Member, action: Json_) -> String {
        let (status, answer) = self.try_act(member, action).await;
        assert_ne!(status, StatusCode::OK, "{answer}");
        reason(&answer).to_owned()
    }

    fn card(&self, member: &Member) -> String {
        sealed(&self.bridge, &member.discord, self.now())
    }

    async fn ask(&self, from: &Member, to: &Member) -> Json_ {
        let card = self.card(to);
        self.act(from, json!({ "action": "request", "card": card, "label": to.name }))
            .await
    }

    async fn befriend(&self, one: &Member, other: &Member) -> String {
        self.ask(one, other).await;
        let request = self.sync(other, menus()).await["incoming"][0]["id"].clone();
        let state = self
            .act(other, json!({ "action": "accept", "request": request }))
            .await;
        state["friends"][0]["id"].as_str().unwrap().to_owned()
    }

    async fn cards(&self, member: &Member, selectors: Json_) -> Json_ {
        let (status, answer) = self
            .post(member, LOOKUP_PATH, json!({ "cards": selectors }))
            .await;
        assert_eq!(status, StatusCode::OK, "{answer}");
        answer["cards"].clone()
    }

    /// `viewer`'s card of `member`: its relation, or null.
    async fn relation(&self, viewer: &Member, member: &Member) -> Json_ {
        let card = self.card(member);
        self.cards(viewer, json!([{ "card": card }])).await[0]["relation"].clone()
    }

    async fn sql(&self, statement: String) {
        self.bridge
            .state()
            .db
            .write(move |tx| {
                tx.execute_batch(&statement)?;
                Ok(())
            })
            .await
            .unwrap();
    }

    async fn count(&self, query: &'static str, account: &str) -> i64 {
        let account = account.to_owned();
        self.bridge
            .state()
            .db
            .read(move |tx| Ok(tx.query_row(query, [account], |row| row.get(0))?))
            .await
            .unwrap()
    }

    /// A public room `creator` opened, with `members` in it.
    async fn room(&self, creator: &Member, members: u32) -> String {
        let room_id = open_room(&self.bridge, &creator.player, "198.51.100.1", 8).await;
        report(&self.fake, &room_id, members, 1, &[]);
        ember_bridge::poll_rooms(self.bridge.state()).await;
        room_id
    }
}

const A: &str = "381906487192731648";
const B: &str = "512334908127441920";
const C: &str = "690012345678901234";
const D: &str = "745600011122233344";

/// Whether `bridge` lists `feature` among its capabilities.
async fn offers(bridge: &Bridge, feature: &str) -> bool {
    let response = bridge
        .client
        .get(bridge.url("/v1/capabilities"))
        .send()
        .await
        .unwrap();
    let (_, body) = common::read(response).await;
    body["features"].as_array().unwrap().contains(&json!(feature))
}

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
    let (status, answer) = world
        .try_act(&first, json!({ "action": "new_code" }))
        .await;
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
    assert_eq!((incoming["name"].clone(), incoming["country"].clone()), (json!("Player 1"), json!("GB")));
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
    assert_eq!(world.cards(&c, json!([{ "friend": friend }])).await, json!([null]));

    // By code, as typed: any case, spaces, O for 0. The label is the code.
    let code = friends["code"].as_str().unwrap().to_owned();
    let typed = code.to_lowercase().replace('-', " ").replace('0', "o");
    let asked = world.act(&c, json!({ "action": "request", "code": typed })).await;
    assert_eq!(asked["outgoing"][0]["label"], code);
    assert_eq!(world.sync(&a, menus()).await["incoming"][0]["name"], "Player 3");

    let refusals = [
        (json!({ "action": "request", "code": "0000-0000" }), "unknown_code"),
        (json!({ "action": "request", "code": code }), "own_card"),
        (json!({ "action": "request", "card": world.card(&a), "label": "Me" }), "own_card"),
        (
            json!({ "action": "request", "label": "Player 2",
                "card": sealed(&world.bridge, B, world.now() - CARD_VALID_SECS - 1) }),
            "card_expired",
        ),
    ];
    for (action, expected) in refusals {
        assert_eq!(world.refused(&a, action.clone()).await, expected, "{action}");
    }
}

#[tokio::test]
async fn asking_someone_who_asked_you_makes_you_friends() {
    let world = World::start().await;
    let (a, b) = (world.join(1, A, "Player 1").await, world.join(2, B, "Player 2").await);
    world.ask(&a, &b).await;
    let code = world.sync(&a, menus()).await["code"].clone();
    let both = world.act(&b, json!({ "action": "request", "code": code })).await;
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
        .act(&b, json!({ "action": "block", "card": card, "label": "Player 3" }))
        .await;
    let code = world.sync(&b, menus()).await["code"].clone();
    let asked = world.act(&c, json!({ "action": "request", "code": code })).await;
    assert_eq!(ids(&asked["outgoing"]).len(), 1);
    assert!(ids(&world.sync(&b, menus()).await["incoming"]).is_empty());
    assert_eq!(world.relation(&b, &c).await, "blocked");
    let blocker = world.card(&b);
    assert_eq!(world.cards(&c, json!([{ "card": blocker }])).await, json!([null]));

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
    assert!(inbox["incoming"].as_array().unwrap().iter().all(|r| r["name"] == "Player 9"));

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
        .act(&a, json!({ "action": "cancel", "request": request_id(100) }))
        .await;
    assert_eq!(ids(&world.act(&a, ask_c).await["outgoing"]).len(), 30);
}

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
        .act(&a, json!({ "action": "invite", "friend": friend,
            "room": { "room_id": "0f0123456789abcdef0123456789abcd" } }))
        .await;
    assert_eq!(ids(&world.sync(&b, menus()).await["invites"]).len(), 1);

    let removed = world
        .act(&a, json!({ "action": "remove", "friend": friend, "block": true }))
        .await;
    assert!(ids(&removed["friends"]).is_empty());
    assert_eq!(removed["blocked"][0]["name"], "Player 2");
    let blocked = world.sync(&b, menus()).await;
    assert!(ids(&blocked["friends"]).is_empty() && ids(&blocked["invites"]).is_empty());
    assert!(!blocked.to_string().contains("Player 1"), "{blocked}");
    assert_eq!(world.relation(&a, &b).await, "blocked");
    let blocker = world.card(&a);
    assert_eq!(world.cards(&b, json!([{ "card": blocker }])).await, json!([null]));
    // The blocked player's friend id no longer opens a card either.
    assert_eq!(world.cards(&b, json!([{ "friend": friend }])).await, json!([null]));

    // Decline and block lists the sender by their own name.
    world.ask(&c, &a).await;
    let request = world.sync(&a, menus()).await["incoming"][0]["id"].clone();
    let declined = world
        .act(&a, json!({ "action": "decline", "request": request, "block": true }))
        .await;
    let names: Vec<&Json_> = declined["blocked"].as_array().unwrap().iter().map(|b| &b["name"]).collect();
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

#[tokio::test]
async fn turning_friends_off_deletes_the_players_friends_data() {
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
        .act(&a, json!({ "action": "block", "card": card, "label": "Player 3" }))
        .await;
    world.ask(&d, &a).await;
    // C's own block on A is C's to keep.
    let card = world.card(&a);
    world
        .act(&c, json!({ "action": "block", "card": card, "label": "Player 1" }))
        .await;
    let old = world.sync(&a, menus()).await["code"].clone();

    let forgotten = world.act(&a, json!({ "action": "forget" })).await;
    assert_ne!(forgotten["code"], old);
    for list in ["friends", "incoming", "outgoing", "invites", "blocked"] {
        assert!(ids(&forgotten[list]).is_empty(), "{list}");
    }
    assert!(ids(&world.sync(&b, menus()).await["friends"]).is_empty());
    assert!(ids(&world.sync(&d, menus()).await["outgoing"]).is_empty());
    assert_eq!(
        world.refused(&b, json!({ "action": "request", "code": old })).await,
        "unknown_code"
    );
    for (query, rows) in [
        ("SELECT COUNT(*) FROM friendships WHERE first = ?1 OR second = ?1", 0),
        ("SELECT COUNT(*) FROM friend_requests WHERE sender = ?1 OR receiver = ?1", 0),
        ("SELECT COUNT(*) FROM friend_blocks WHERE blocker = ?1", 0),
        ("SELECT COUNT(*) FROM friend_blocks WHERE blocked = ?1", 1),
    ] {
        assert_eq!(world.count(query, A).await, rows, "{query}");
    }
}

#[tokio::test]
async fn a_friends_public_room_is_joinable_on_the_same_build() {
    let world = World::start().await;
    let (a, b) = (world.join(1, A, "Player 1").await, world.join(2, B, "Player 2").await);
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
    assert_eq!(presence(world.sync(&a, menus()).await)["room"]["joinable"], false);

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

#[tokio::test]
async fn appearing_offline_freezes_what_friends_see() {
    let world = World::start().await;
    let (mut a, mut b, c) = (
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
        world.sync(&b, menus()).await;
        assert_eq!(presence(world.sync(&a, menus()).await), offline);
    }
    // Nobody can invite a friend who appears offline; they can invite.
    let a_friend = world.sync(&b, menus()).await["friends"][0]["id"].clone();
    let room = json!({ "room_id": "0f0123456789abcdef0123456789abcd" });
    assert_eq!(
        world
            .refused(&a, json!({ "action": "invite", "friend": friend, "room": room }))
            .await,
        "invite_unavailable"
    );
    let invited = world
        .act(&b, json!({ "action": "invite", "friend": a_friend, "room": room }))
        .await;
    assert!(ids(&invited["invites"]).is_empty());
    let invites = world.sync(&a, menus()).await["invites"].clone();
    assert_eq!(invites[0]["friend"], friend);
    assert_eq!(invites[0]["room"], room);
    // Not a friend: nobody to invite.
    assert_eq!(
        world
            .refused(&c, json!({ "action": "invite", "friend": friend, "room": room }))
            .await,
        "invite_unavailable"
    );
    let dismissed = world
        .act(&a, json!({ "action": "dismiss", "invite": invites[0]["id"] }))
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
    let (mut a, mut b) = (world.join(1, A, "Player 1").await, world.join(2, B, "Player 2").await);
    let old = world.sync(&a, menus()).await["code"].clone();
    assert_eq!(world.refused(&a, json!({ "action": "new_code" })).await, "code_too_soon");
    world.bridge.clock.advance(DAY);
    world.renew(&mut a).await;
    world.renew(&mut b).await;
    let new = world.act(&a, json!({ "action": "new_code" })).await["code"].clone();
    assert_ne!(new, old);
    assert_eq!(
        world.refused(&b, json!({ "action": "request", "code": old })).await,
        "unknown_code"
    );
    world.act(&b, json!({ "action": "request", "code": new })).await;
    assert_eq!(world.sync(&a, menus()).await["incoming"][0]["name"], "Player 2");
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
        .sync(&b, json!({ "activity": "room",
            "room": { "room_id": room_id, "status": "watching", "table": 1 } }))
        .await;
    let secret = format!("emd2:{}", "a-_9".repeat(30) + "ab");
    world
        .act(&a, json!({ "action": "invite", "friend": friend, "room": { "secret": secret } }))
        .await;
    let invite = world.sync(&b, menus()).await["invites"][0]["id"].clone();
    world.act(&b, json!({ "action": "dismiss", "invite": invite })).await;
    world.ask(&c, &a).await;
    let request = world.sync(&a, menus()).await["incoming"][0]["id"].clone();
    world
        .act(&a, json!({ "action": "decline", "request": request }))
        .await;
    let sent = world.sync(&c, menus()).await["outgoing"][0]["id"].clone();
    world.act(&c, json!({ "action": "cancel", "request": sent })).await;
    let card = world.card(&c);
    let blocked = world
        .act(&b, json!({ "action": "block", "card": card, "label": "Player 3" }))
        .await;
    world
        .act(&b, json!({ "action": "unblock", "block": blocked["blocked"][0]["id"] }))
        .await;
    world
        .act(&a, json!({ "action": "appear_offline", "on": true }))
        .await;
    world.refused(&a, json!({ "action": "new_code" })).await;
    world.cards(&b, json!([{ "card": world.card(&a) }, { "friend": friend }])).await;
    world
        .act(&b, json!({ "action": "remove", "friend": friend }))
        .await;
    world.act(&b, json!({ "action": "forget" })).await;
    for path in [POINTS_PATH, STANDING_PATH, BOARD_PATH, "/v1/rooms?detail=1"] {
        let (status, answer) = world.get(&a, path).await;
        assert_eq!(status, StatusCode::OK, "{path}: {answer}");
    }
    let ticket = format!("/v1/rooms/{room_id}/tickets");
    let (status, answer) = world
        .post(&b, &ticket, json!({ "endpoint_id": ENDPOINT, "build_id": BUILD, "card": true }))
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
    for path in [SYNC_PATH, ACTIONS_PATH, LOOKUP_PATH, POINTS_PATH, STANDING_PATH, BOARD_PATH] {
        assert!(paths.contains(path), "{path} was not called");
    }
}
