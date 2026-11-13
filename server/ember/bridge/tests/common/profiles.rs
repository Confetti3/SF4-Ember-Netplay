//! A bridge with public rooms and Discord sign-in, and the players of the
//! profile tests (`profiles.rs`, `profile_search.rs`, `profile_cards.rs`):
//! every answer a test receives is kept, and scanned when its `World` ends for
//! the Discord IDs and names it seeded, and for the Ember IDs: none may carry
//! one.
use std::sync::Mutex;

use super::{
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

pub const IP: &str = "198.51.100.7";
/// When every member connected Discord: 2025-06-01 plus a few hours.
pub const CONNECTED: u64 = 1_748_736_000 + 64_000;
/// The first second of that month.
pub const MONTH: u64 = 1_748_736_000;

/// A Discord account ID no real one is.
pub fn account(n: u64) -> String {
    (381_906_487_192_731_648u64 + n).to_string()
}

/// A `player_cards` row: name, country, main, since, device, updated_at.
pub type Stored = (String, Option<String>, Option<i64>, i64, i64, i64);

/// A player with a Discord account.
pub struct Member {
    pub player: Player,
    pub discord: String,
    pub name: &'static str,
}

/// A bridge with public rooms and Discord sign-in, whose clock moves only
/// when a test moves it, and every answer it gave.
pub struct World {
    pub bridge: Bridge,
    pub _fake: Supervisor,
    /// (path, answer) of every request a test made.
    pub answers: Mutex<Vec<(String, String)>>,
    /// The Discord IDs and names seeded: in no answer at all.
    pub accounts: Mutex<Vec<String>>,
    /// The Ember IDs seeded: in no answer at all.
    pub ember_ids: Mutex<Vec<String>>,
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
    pub async fn start() -> Self {
        Self::start_with(true, false).await
    }

    /// A bridge whose `rooms.friends` switch is `friends` and whose
    /// `rooms.skill_badges` is `classes`.
    pub async fn start_with(friends: bool, classes: bool) -> Self {
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

    pub fn now(&self) -> u64 {
        self.bridge.state().now()
    }

    /// One more second, so the cards saved next are the newer.
    pub fn tick(&self) {
        self.bridge.clock.advance(1);
    }

    /// A player with Discord account number `n`, connected at `CONNECTED`.
    pub async fn member(&self, byte: u8, n: u64, name: &'static str) -> Member {
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
    pub async fn joined(&self, byte: u8, n: u64, name: &'static str) -> Member {
        let member = self.member(byte, n, name).await;
        self.sync(&member).await;
        member
    }

    pub fn record(&self, path: &str, answer: &Json_) {
        self.answers
            .lock()
            .unwrap()
            .push((path.to_owned(), answer.to_string()));
    }

    pub async fn call(
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
    pub async fn save(&self, member: &Member, card: Json_) {
        let (status, answer) = self.try_save(member, card).await;
        assert_eq!(status, StatusCode::NO_CONTENT, "{answer}");
        assert_eq!(answer, Json_::Null);
    }

    pub async fn try_save(&self, member: &Member, card: Json_) -> (StatusCode, Json_) {
        self.call(Method::PUT, member, CARDS_ME_PATH, Some(card)).await
    }

    /// The names of the players `body` finds for `member`, in order, and
    /// whether more matched; the answer is checked as a client reads it.
    pub async fn found(&self, member: &Member, body: Json_) -> (Vec<String>, bool) {
        let answer = self.search(member, body).await;
        let names = answer["players"]
            .as_array()
            .unwrap()
            .iter()
            .map(|hit| hit["name"].as_str().unwrap().to_owned())
            .collect();
        (names, answer["more"].as_bool().unwrap())
    }

    pub async fn search(&self, member: &Member, body: Json_) -> Json_ {
        let (status, answer) = self.try_search(member, body).await;
        assert_eq!(status, StatusCode::OK, "{answer}");
        let parsed: SearchAnswer = serde_json::from_value(answer.clone()).unwrap();
        parsed.check().unwrap();
        answer
    }

    pub async fn try_search(&self, member: &Member, body: Json_) -> (StatusCode, Json_) {
        self.call(Method::POST, member, SEARCH_PATH, Some(body)).await
    }

    /// The cards `member` sees for `selectors`, each checked as a client
    /// reads it.
    pub async fn cards(&self, member: &Member, selectors: Json_) -> Json_ {
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
    pub fn token(&self, member: &Member) -> String {
        let mut plain = vec![0x01];
        plain.extend(member.discord.parse::<u64>().unwrap().to_le_bytes());
        plain.extend(self.now().to_le_bytes());
        format!("{CARD_PREFIX}{}", b64u(&self.bridge.state().keys.seal(&plain)))
    }

    /// `viewer`'s card of `member`, or null.
    pub async fn card_of(&self, viewer: &Member, member: &Member) -> Json_ {
        let token = self.token(member);
        self.cards(viewer, json!([{ "card": token }])).await[0].clone()
    }

    pub async fn sync_as(&self, member: &Member, name: &str, country: &str, main: u8) -> Json_ {
        let body = json!({ "build_id": BUILD, "listing": { "name": name, "country": country },
            "main": main, "presence": { "activity": "menus" } });
        let (status, answer) = self.call(Method::POST, member, SYNC_PATH, Some(body)).await;
        assert_eq!(status, StatusCode::OK, "{answer}");
        answer
    }

    pub async fn sync(&self, member: &Member) -> Json_ {
        self.sync_as(member, member.name, "GB", 3).await
    }

    pub async fn act(&self, member: &Member, action: Json_) -> Json_ {
        let (status, answer) = self
            .call(Method::POST, member, ACTIONS_PATH, Some(action))
            .await;
        assert_eq!(status, StatusCode::OK, "{answer}");
        answer
    }

    /// `from` asks `to` by card, and `to` answers it: the request id.
    pub async fn ask(&self, from: &Member, to: &Member) -> String {
        let card = self.token(to);
        self.act(from, json!({ "action": "request", "card": card, "label": to.name }))
            .await;
        self.sync(to).await["incoming"][0]["id"]
            .as_str()
            .unwrap()
            .to_owned()
    }

    /// Makes `one` and `other` friends: the friendship's id.
    pub async fn befriend(&self, one: &Member, other: &Member) -> String {
        let request = self.ask(one, other).await;
        let state = self
            .act(other, json!({ "action": "accept", "request": request }))
            .await;
        state["friends"][0]["id"].as_str().unwrap().to_owned()
    }

    pub async fn sql(&self, statement: String) {
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
    pub async fn code_of(&self, member: &Member) -> String {
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
    pub async fn stored(&self, discord: &str) -> Option<Stored> {
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

pub async fn offers(bridge: &Bridge, feature: &str) -> bool {
    let response = bridge
        .client
        .get(bridge.url("/v1/capabilities"))
        .send()
        .await
        .unwrap();
    let (_, body) = super::read(response).await;
    body["features"].as_array().unwrap().contains(&json!(feature))
}

pub fn full_card() -> Json_ {
    json!({ "listing": { "name": "Player 1", "country": "DE" }, "main": 3,
        "worn": { "title": "matches", "icon": "streak" }, "pins": ["rank", "wins"],
        "style": { "device": 2, "link": 1, "region": "euc1", "looking": 34 } })
}
