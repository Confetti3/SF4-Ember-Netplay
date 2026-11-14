//! The friends tests' bridge and players (`friends.rs`,
//! `friends_blocks.rs`, `friends_presence.rs`): every answer a test receives
//! is kept, and scanned when its `World` ends for the Discord IDs and names it
//! seeded, and the friends and card answers also for the Ember IDs: none may
//! carry one.
use std::{collections::BTreeSet, sync::Mutex};

use super::{
    Bridge, Player,
    public_rooms::{open_room, player, reason},
    seed,
    supervisor::{self, BUILD, Supervisor, report},
};
use ember_bridge::{config, integrations::Secrets};
use ember_protocol::{
    encoding::b64u,
    friends::{ACTIONS_PATH, FriendAction, FriendsState, SYNC_PATH},
    levels::{CARD_PREFIX, LOOKUP_PATH},
};
use reqwest::StatusCode;
use serde_json::{Value as Json_, json};
use zeroize::Zeroizing;

pub const DAY: i64 = 24 * 60 * 60;

/// Each action's name, by a match without a wildcard: a new action does not
/// build until it is named here, and `every_action_and_route_is_scanned`
/// must then send it (`ACTIONS`).
pub fn named(action: &FriendAction) -> &'static str {
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
        FriendAction::TurnOn {} => "turn_on",
    }
}

pub const ACTIONS: [&str; 13] = [
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
    "turn_on",
];

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
    pub fake: Supervisor,
    /// (path, answer) of every request a test made.
    pub answers: Mutex<Vec<(String, String)>>,
    /// The Discord IDs and names seeded: in no answer at all.
    pub accounts: Mutex<Vec<String>>,
    /// The Ember IDs seeded: in no friends or card answer.
    pub ember_ids: Mutex<Vec<String>>,
    /// The actions sent, by name.
    pub sent: Mutex<BTreeSet<&'static str>>,
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
                assert!(
                    !answer.contains(secret.as_str()),
                    "{path} names {secret}: {answer}"
                );
            }
            if path.starts_with("/v1/friends") || path == LOOKUP_PATH {
                for ember_id in ember_ids.iter() {
                    assert!(
                        !answer.contains(ember_id.as_str()),
                        "{path} names {ember_id}: {answer}"
                    );
                }
            }
        }
    }
}

/// Seals a card token for `discord` issued at `at`, as `cards::issue` does.
pub fn sealed(bridge: &Bridge, discord: &str, at: u64) -> String {
    let mut plain = vec![0x01];
    plain.extend(discord.parse::<u64>().unwrap().to_le_bytes());
    plain.extend(at.to_le_bytes());
    format!("{CARD_PREFIX}{}", b64u(&bridge.state().keys.seal(&plain)))
}

pub fn menus() -> Json_ {
    json!({ "activity": "menus" })
}

/// The ids of a list in a state.
pub fn ids(list: &Json_) -> Vec<String> {
    list.as_array()
        .unwrap()
        .iter()
        .map(|entry| entry["id"].as_str().unwrap().to_owned())
        .collect()
}

impl World {
    pub async fn start() -> Self {
        Self::start_with(true).await
    }

    /// A bridge whose `rooms.friends` switch is `friends`.
    pub async fn start_with(friends: bool) -> Self {
        let (fake, url) = supervisor::fake_supervisor().await;
        let bridge = Bridge::start_with(
            |config| {
                supervisor::enable(config, url);
                config.rooms.as_mut().unwrap().friends = friends;
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

    pub fn now(&self) -> u64 {
        self.bridge.state().now()
    }

    /// A player with Discord account `discord`, who has synced once.
    pub async fn join(&self, byte: u8, discord: &str, name: &'static str) -> Member {
        let member = self.member(byte, discord, name).await;
        self.sync(&member, menus()).await;
        member
    }

    /// A player with Discord account `discord`, who never synced.
    pub async fn member(&self, byte: u8, discord: &str, name: &'static str) -> Member {
        let player = player(&self.bridge, byte).await;
        let username = format!("discord_name_{discord}");
        seed::link_discord(
            self.bridge.state(),
            discord,
            player.id().as_str(),
            &username,
        )
        .await;
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
    pub async fn renew(&self, member: &mut Member) {
        self.bridge.open_session(&mut member.player).await;
    }

    pub fn record(&self, path: &str, answer: &Json_) {
        self.answers
            .lock()
            .unwrap()
            .push((path.to_owned(), answer.to_string()));
    }

    pub async fn post(&self, member: &Member, path: &str, body: Json_) -> (StatusCode, Json_) {
        let (status, answer) = self.bridge.post(member.player.token(), path, body).await;
        self.record(path, &answer);
        (status, answer)
    }

    pub async fn get(&self, member: &Member, path: &str) -> (StatusCode, Json_) {
        let (status, answer) = self.bridge.get(member.player.token(), path).await;
        self.record(path, &answer);
        (status, answer)
    }

    /// The state, checked as a client reads it.
    pub fn state_of(status: StatusCode, answer: Json_) -> Json_ {
        assert_eq!(status, StatusCode::OK, "{answer}");
        let state: FriendsState = serde_json::from_value(answer.clone()).unwrap();
        state.check().unwrap();
        answer
    }

    pub async fn sync_on(&self, member: &Member, build: &str, presence: Json_) -> Json_ {
        let body = json!({ "build_id": build, "listing": { "name": member.name, "country": "GB" },
            "main": 3, "presence": presence });
        let (status, answer) = self.post(member, SYNC_PATH, body).await;
        Self::state_of(status, answer)
    }

    pub async fn sync(&self, member: &Member, presence: Json_) -> Json_ {
        self.sync_on(member, BUILD, presence).await
    }

    pub async fn try_act(&self, member: &Member, action: Json_) -> (StatusCode, Json_) {
        let parsed: FriendAction = serde_json::from_value(action.clone()).unwrap();
        self.sent.lock().unwrap().insert(named(&parsed));
        self.post(member, ACTIONS_PATH, action).await
    }

    pub async fn act(&self, member: &Member, action: Json_) -> Json_ {
        let (status, answer) = self.try_act(member, action).await;
        Self::state_of(status, answer)
    }

    /// The refusal's reason for an action.
    pub async fn refused(&self, member: &Member, action: Json_) -> String {
        let (status, answer) = self.try_act(member, action).await;
        assert_ne!(status, StatusCode::OK, "{answer}");
        reason(&answer).to_owned()
    }

    pub fn card(&self, member: &Member) -> String {
        sealed(&self.bridge, &member.discord, self.now())
    }

    pub async fn ask(&self, from: &Member, to: &Member) -> Json_ {
        let card = self.card(to);
        self.act(
            from,
            json!({ "action": "request", "card": card, "label": to.name }),
        )
        .await
    }

    pub async fn befriend(&self, one: &Member, other: &Member) -> String {
        self.ask(one, other).await;
        let request = self.sync(other, menus()).await["incoming"][0]["id"].clone();
        let state = self
            .act(other, json!({ "action": "accept", "request": request }))
            .await;
        state["friends"][0]["id"].as_str().unwrap().to_owned()
    }

    pub async fn cards(&self, member: &Member, selectors: Json_) -> Json_ {
        let (status, answer) = self
            .post(member, LOOKUP_PATH, json!({ "cards": selectors }))
            .await;
        assert_eq!(status, StatusCode::OK, "{answer}");
        answer["cards"].clone()
    }

    /// `viewer`'s card of `member`: its relation, or null.
    pub async fn relation(&self, viewer: &Member, member: &Member) -> Json_ {
        let card = self.card(member);
        self.cards(viewer, json!([{ "card": card }])).await[0]["relation"].clone()
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

    pub async fn count(&self, query: &'static str, account: &str) -> i64 {
        let account = account.to_owned();
        self.bridge
            .state()
            .db
            .read(move |tx| Ok(tx.query_row(query, [account], |row| row.get(0))?))
            .await
            .unwrap()
    }

    pub async fn code_of(&self, account: &str) -> String {
        let account = account.to_owned();
        self.bridge
            .state()
            .db
            .read(move |tx| {
                Ok(tx.query_row(
                    "SELECT code FROM social_accounts WHERE discord_user_id = ?1",
                    [account],
                    |row| row.get(0),
                )?)
            })
            .await
            .unwrap()
    }

    /// A public room `creator` opened, with `members` in it.
    pub async fn room(&self, creator: &Member, members: u32) -> String {
        let room_id = open_room(&self.bridge, &creator.player, "198.51.100.1", 8).await;
        report(&self.fake, &room_id, members, 1, &[]);
        ember_bridge::poll_rooms(self.bridge.state()).await;
        room_id
    }
}

pub const A: &str = "381906487192731648";
pub const B: &str = "512334908127441920";
pub const C: &str = "690012345678901234";
pub const D: &str = "745600011122233344";

/// Whether `bridge` lists `feature` among its capabilities.
pub async fn offers(bridge: &Bridge, feature: &str) -> bool {
    let response = bridge
        .client
        .get(bridge.url("/v1/capabilities"))
        .send()
        .await
        .unwrap();
    let (_, body) = super::read(response).await;
    body["features"]
        .as_array()
        .unwrap()
        .contains(&json!(feature))
}
