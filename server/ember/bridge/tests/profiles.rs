//! Profiles (docs/design/PROFILE.md): a player saves what their card shows,
//! anyone with a selector reads it, and players are found by name or friend
//! code. The card look-up opens a received request and a friend code too, and
//! says the caller's record and friendship with the player. Migration 064's
//! test is in `levels_migrations.rs`.
//!
//! Every answer a test here receives is kept, and scanned when its `World`
//! ends for the Discord IDs and names it seeded, and for the Ember IDs: none
//! may carry one.
mod common;

use std::sync::Mutex;

use common::{
    Bridge, Player,
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

const IP: &str = "198.51.100.7";
/// When every member connected Discord: 2025-06-01 plus a few hours.
const CONNECTED: u64 = 1_748_736_000 + 64_000;
/// The first second of that month.
const MONTH: u64 = 1_748_736_000;

/// A Discord account ID no real one is.
fn account(n: u64) -> String {
    (381_906_487_192_731_648u64 + n).to_string()
}

/// A `player_cards` row: name, country, main, since, device, updated_at.
type Stored = (String, Option<String>, Option<i64>, i64, i64, i64);

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
    _fake: Supervisor,
    /// (path, answer) of every request a test made.
    answers: Mutex<Vec<(String, String)>>,
    /// The Discord IDs and names seeded: in no answer at all.
    accounts: Mutex<Vec<String>>,
    /// The Ember IDs seeded: in no answer at all.
    ember_ids: Mutex<Vec<String>>,
}

impl Drop for World {
    fn drop(&mut self) {
        if std::thread::panicking() {
            return;
        }
        let accounts = self.accounts.lock().unwrap();
        let ember_ids = self.ember_ids.lock().unwrap();
        for (path, answer) in self.answers.lock().unwrap().iter() {
            for secret in accounts.iter().chain(ember_ids.iter()) {
                assert!(!answer.contains(secret.as_str()), "{path} names {secret}: {answer}");
            }
        }
    }
}

impl World {
    async fn start() -> Self {
        Self::start_with(true, false).await
    }

    /// A bridge whose `rooms.friends` switch is `friends` and whose
    /// `rooms.skill_badges` is `classes`.
    async fn start_with(friends: bool, classes: bool) -> Self {
        let (fake, url) = supervisor::fake_supervisor().await;
        let bridge = Bridge::start_with(
            |config| {
                supervisor::enable(config, url);
                let rooms = config.rooms.as_mut().unwrap();
                rooms.friends = friends;
                rooms.skill_badges = classes;
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
            _fake: fake,
            answers: Mutex::default(),
            accounts: Mutex::default(),
            ember_ids: Mutex::default(),
        }
    }

    fn now(&self) -> u64 {
        self.bridge.state().now()
    }

    /// One more second, so the cards saved next are the newer.
    fn tick(&self) {
        self.bridge.clock.advance(1);
    }

    /// A player with Discord account number `n`, connected at `CONNECTED`.
    async fn member(&self, byte: u8, n: u64, name: &'static str) -> Member {
        let player = player(&self.bridge, byte).await;
        let discord = account(n);
        let username = format!("discord_name_{discord}");
        seed::link_discord_at(
            self.bridge.state(),
            &discord,
            player.id().as_str(),
            &username,
            CONNECTED,
        )
        .await;
        self.accounts.lock().unwrap().extend([discord.clone(), username]);
        self.ember_ids
            .lock()
            .unwrap()
            .push(player.id().as_str().to_owned());
        Member {
            player,
            discord,
            name,
        }
    }

    /// A player with a Discord account who has synced once.
    async fn joined(&self, byte: u8, n: u64, name: &'static str) -> Member {
        let member = self.member(byte, n, name).await;
        self.sync(&member).await;
        member
    }

    fn record(&self, path: &str, answer: &Json_) {
        self.answers
            .lock()
            .unwrap()
            .push((path.to_owned(), answer.to_string()));
    }

    async fn call(
        &self,
        method: Method,
        member: &Member,
        path: &str,
        body: Option<Json_>,
    ) -> (StatusCode, Json_) {
        let (status, answer) = send(
            &self.bridge,
            method,
            member.player.token(),
            IP,
            path,
            body,
        )
        .await;
        self.record(path, &answer);
        (status, answer)
    }

    /// Saves `card` as `member`, expecting 204.
    async fn save(&self, member: &Member, card: Json_) {
        let (status, answer) = self.try_save(member, card).await;
        assert_eq!(status, StatusCode::NO_CONTENT, "{answer}");
        assert_eq!(answer, Json_::Null);
    }

    async fn try_save(&self, member: &Member, card: Json_) -> (StatusCode, Json_) {
        self.call(Method::PUT, member, CARDS_ME_PATH, Some(card)).await
    }

    /// The names of the players `body` finds for `member`, in order, and
    /// whether more matched; the answer is checked as a client reads it.
    async fn found(&self, member: &Member, body: Json_) -> (Vec<String>, bool) {
        let answer = self.search(member, body).await;
        let names = answer["players"]
            .as_array()
            .unwrap()
            .iter()
            .map(|hit| hit["name"].as_str().unwrap().to_owned())
            .collect();
        (names, answer["more"].as_bool().unwrap())
    }

    async fn search(&self, member: &Member, body: Json_) -> Json_ {
        let (status, answer) = self.try_search(member, body).await;
        assert_eq!(status, StatusCode::OK, "{answer}");
        let parsed: SearchAnswer = serde_json::from_value(answer.clone()).unwrap();
        parsed.check().unwrap();
        answer
    }

    async fn try_search(&self, member: &Member, body: Json_) -> (StatusCode, Json_) {
        self.call(Method::POST, member, SEARCH_PATH, Some(body)).await
    }

    /// The cards `member` sees for `selectors`, each checked as a client
    /// reads it.
    async fn cards(&self, member: &Member, selectors: Json_) -> Json_ {
        let (status, answer) = self
            .call(
                Method::POST,
                member,
                LOOKUP_PATH,
                Some(json!({ "cards": selectors })),
            )
            .await;
        assert_eq!(status, StatusCode::OK, "{answer}");
        let parsed: CardAnswers = serde_json::from_value(answer.clone()).unwrap();
        parsed.check().unwrap();
        answer["cards"].clone()
    }

    /// A fresh card token of `member`, as a room would give it.
    fn token(&self, member: &Member) -> String {
        let mut plain = vec![0x01];
        plain.extend(member.discord.parse::<u64>().unwrap().to_le_bytes());
        plain.extend(self.now().to_le_bytes());
        format!("{CARD_PREFIX}{}", b64u(&self.bridge.state().keys.seal(&plain)))
    }

    /// `viewer`'s card of `member`, or null.
    async fn card_of(&self, viewer: &Member, member: &Member) -> Json_ {
        let token = self.token(member);
        self.cards(viewer, json!([{ "card": token }])).await[0].clone()
    }

    async fn sync_as(&self, member: &Member, name: &str, country: &str, main: u8) -> Json_ {
        let body = json!({ "build_id": BUILD, "listing": { "name": name, "country": country },
            "main": main, "presence": { "activity": "menus" } });
        let (status, answer) = self.call(Method::POST, member, SYNC_PATH, Some(body)).await;
        assert_eq!(status, StatusCode::OK, "{answer}");
        answer
    }

    async fn sync(&self, member: &Member) -> Json_ {
        self.sync_as(member, member.name, "GB", 3).await
    }

    async fn act(&self, member: &Member, action: Json_) -> Json_ {
        let (status, answer) = self
            .call(Method::POST, member, ACTIONS_PATH, Some(action))
            .await;
        assert_eq!(status, StatusCode::OK, "{answer}");
        answer
    }

    /// `from` asks `to` by card, and `to` answers it: the request id.
    async fn ask(&self, from: &Member, to: &Member) -> String {
        let card = self.token(to);
        self.act(from, json!({ "action": "request", "card": card, "label": to.name }))
            .await;
        self.sync(to).await["incoming"][0]["id"]
            .as_str()
            .unwrap()
            .to_owned()
    }

    /// Makes `one` and `other` friends: the friendship's id.
    async fn befriend(&self, one: &Member, other: &Member) -> String {
        let request = self.ask(one, other).await;
        let state = self
            .act(other, json!({ "action": "accept", "request": request }))
            .await;
        state["friends"][0]["id"].as_str().unwrap().to_owned()
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

    /// `member`'s friend code, as the bridge stored it.
    async fn code_of(&self, member: &Member) -> String {
        let discord = member.discord.clone();
        self.bridge
            .state()
            .db
            .read(move |tx| {
                Ok(tx.query_row(
                    "SELECT code FROM social_accounts WHERE discord_user_id = ?1",
                    [discord],
                    |row| row.get(0),
                )?)
            })
            .await
            .unwrap()
    }

    /// `player_cards`' (name, country, main, since, device, updated_at) of
    /// `discord`.
    async fn stored(&self, discord: &str) -> Option<Stored> {
        let discord = discord.to_owned();
        self.bridge
            .state()
            .db
            .read(move |tx| {
                Ok(tx
                    .query_row(
                        "SELECT name, country, main_fighter, since, device, updated_at
                         FROM player_cards WHERE discord_user_id = ?1",
                        [discord],
                        |row| {
                            Ok((
                                row.get(0)?,
                                row.get(1)?,
                                row.get(2)?,
                                row.get(3)?,
                                row.get(4)?,
                                row.get(5)?,
                            ))
                        },
                    )
                    .optional()?)
            })
            .await
            .unwrap()
    }
}

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

fn full_card() -> Json_ {
    json!({ "listing": { "name": "Player 1", "country": "DE" }, "main": 3,
        "worn": { "title": "matches", "icon": "streak" }, "pins": ["rank", "wins"],
        "style": { "device": 2, "link": 1, "region": "euc1", "looking": 34 } })
}

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

/// Names saved in this order, a second apart: the most recent is last.
async fn named(world: &World, names: &[&'static str], first: u8, first_account: u64) -> Vec<Member> {
    let mut members = Vec::new();
    for (at, name) in names.iter().copied().enumerate() {
        let member = world
            .member(first + at as u8, first_account + at as u64, name)
            .await;
        world.tick();
        world.save(&member, json!({ "listing": { "name": name } })).await;
        members.push(member);
    }
    members
}

#[tokio::test]
async fn a_name_finds_exact_matches_first_then_leading_ones_each_newest_first() {
    let world = World::start().await;
    let caller = world.member(1, 1, "Annabel").await;
    world.save(&caller, json!({ "listing": { "name": "Annabel" } })).await;
    // Saved oldest first. "Joann" holds the text but does not begin with it.
    named(&world, &["Ann", "Annie", "ann", "Anna", "Joann", "Bo_b", "Bobb"], 10, 10).await;

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
    world.save(&caller, json!({ "listing": { "name": "Player 1" } })).await;
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
        .sql(statements.replace("'Many 21'", "'Few 21'").replace("'Many 22'", "'Few 22'"))
        .await;
    let (names, more) = world.found(&caller, json!({ "name": "Many" })).await;
    assert_eq!(names.len(), MAX_SEARCH);
    assert!(!more);
    assert_eq!((names[0].as_str(), names[19].as_str()), ("Many 20", "Many 01"));

    // Two more: the twenty newest, and a word that more matched.
    world
        .sql("UPDATE player_cards SET name = REPLACE(name, 'Few', 'Many')".into())
        .await;
    let (names, more) = world.found(&caller, json!({ "name": "Many" })).await;
    assert_eq!(names.len(), MAX_SEARCH);
    assert!(more);
    assert_eq!((names[0].as_str(), names[19].as_str()), ("Many 22", "Many 03"));
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
    let (names, _) = world.found(&a, json!({ "code": world.code_of(&a).await })).await;
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
        .call(Method::POST, &b, ACTIONS_PATH, Some(json!({ "action": "forget" })))
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
    world.save(&b, json!({ "listing": { "name": "Player 2" } })).await;
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
    world.act(&a, json!({ "action": "forget" })).await;
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
