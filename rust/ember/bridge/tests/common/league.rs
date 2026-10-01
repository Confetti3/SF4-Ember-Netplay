//! A provider connection with linked players, for the lobby, record and
//! tournament tests.
use reqwest::StatusCode;
use serde_json::{Value, json};

use super::{Bridge, Player};

pub struct Fixture {
    pub bridge: Bridge,
    pub provider: String,
    pub organizer: String,
    pub players: Vec<Player>,
    pub links: Vec<Value>,
}

pub async fn fixture(count: u8) -> Fixture {
    let bridge = Bridge::start().await;
    let provider = bridge.provider("mock-a").await;
    let organizer = bridge.organizer("t1").await;
    let mut players = Vec::new();
    let mut links = Vec::new();
    for index in 0..count {
        let mut player = bridge.player(60 + index);
        bridge.open_session(&mut player).await;
        links.push(
            bridge
                .link(
                    &provider,
                    &player,
                    "mock-a",
                    &format!("lobby-player-{index}"),
                )
                .await,
        );
        players.push(player);
    }
    Fixture {
        bridge,
        provider,
        organizer,
        players,
        links,
    }
}

impl Fixture {
    pub fn id(&self, index: usize) -> &str {
        self.players[index].id().as_str()
    }

    pub fn participant(&self, index: usize) -> &str {
        self.links[index]["participant_id"].as_str().unwrap()
    }

    pub async fn create(&self, external: &str, games_to_win: u8, rotation: &str) -> Value {
        let (status, lobby) = self
            .bridge
            .post_keyed(
                &self.provider,
                "/v1/lobbies",
                json!({
                    "external_lobby_id": external,
                    "game": "usf4",
                    "games_to_win": games_to_win,
                    "rotation": rotation,
                    "required_build_id": "test-build",
                    "metadata": { "title": "Stream night" },
                }),
                Some(&format!("create-{external}")),
            )
            .await;
        assert_eq!(status, StatusCode::CREATED, "{lobby}");
        lobby
    }

    pub async fn join(&self, lobby: &str, index: usize) -> (StatusCode, Value) {
        self.bridge
            .post_keyed(
                &self.provider,
                &format!("/v1/lobbies/{lobby}/queue"),
                json!({ "participant_id": self.participant(index), "ember_id": self.id(index) }),
                Some(&format!("join-{lobby}-{index}-{}", rand_key())),
            )
            .await
    }

    pub async fn lobby(&self, lobby: &str) -> Value {
        let (status, body) = self
            .bridge
            .get(&self.provider, &format!("/v1/lobbies/{lobby}"))
            .await;
        assert_eq!(status, StatusCode::OK, "{body}");
        body
    }

    /// An ordinary first-to-1 match between two of the players.
    pub async fn bracket(&self, external: &str, first: usize, second: usize) -> Value {
        self.ordinary(external, first, second, 1).await
    }

    /// An ordinary first-to-`games_to_win` match between two of the players.
    pub async fn ordinary(
        &self,
        external: &str,
        first: usize,
        second: usize,
        games_to_win: u8,
    ) -> Value {
        let (status, bracket) = self
            .bridge
            .post_keyed(
                &self.provider,
                "/v1/matches",
                json!({
                    "external_match_id": external,
                    "game": "usf4",
                    "participants": [
                        { "participant_id": self.participant(first), "ember_id": self.id(first), "slot": 0 },
                        { "participant_id": self.participant(second), "ember_id": self.id(second), "slot": 1 },
                    ],
                    "rules": {
                        "games_to_win": games_to_win, "draw_policy": "replay_no_score", "native_rules_profile": "organizer-reported-v1",
                        "edition_policy": "ultra_only", "character_policy": "unrestricted_between_games",
                        "stage_policy": "p1_selects", "input_delay_policy": "ember_existing_ready_policy",
                    },
                    "observer_policy": "authorized_only",
                    "result_policy": "two_player_agreement_or_review",
                    "required_build_id": "test-build",
                    "metadata": {},
                }),
                Some(external),
            )
            .await;
        assert_eq!(status, StatusCode::CREATED, "{bracket}");
        bracket
    }

    /// The organizer records one game of the lobby's running set.
    pub async fn win(&self, lobby: &str, winner_slot: u8) -> (StatusCode, Value) {
        let current = self.lobby(lobby).await;
        let id = current["current_match_id"]
            .as_str()
            .expect("a running set")
            .to_owned();
        self.game(&id, winner_slot).await
    }

    /// The organizer records one game of any match.
    pub async fn game(&self, id: &str, winner_slot: u8) -> (StatusCode, Value) {
        let (_, found) = self
            .bridge
            .get(&self.organizer, &format!("/v1/matches/{id}"))
            .await;
        self.bridge
            .post_keyed(
                &self.organizer,
                &format!("/v1/matches/{id}/adjudications"),
                json!({
                    "kind": "game_result",
                    "winner_slot": winner_slot,
                    "reason": "Called on stream",
                    "expected_revision": found["revision"],
                }),
                Some(&format!("win-{id}-{}", rand_key())),
            )
            .await
    }

    /// The organizer voids an accepted game of a match.
    pub async fn void(&self, id: &str, attempt_id: &str) -> (StatusCode, Value) {
        let (_, found) = self
            .bridge
            .get(&self.organizer, &format!("/v1/matches/{id}"))
            .await;
        self.bridge
            .post_keyed(
                &self.organizer,
                &format!("/v1/matches/{id}/adjudications"),
                json!({
                    "kind": "void_game",
                    "attempt_id": attempt_id,
                    "reason": "Wrong slot",
                    "expected_revision": found["revision"],
                }),
                Some(&format!("void-{id}-{}", rand_key())),
            )
            .await
    }
}

pub fn rand_key() -> String {
    let mut bytes = [0u8; 8];
    getrandom::fill(&mut bytes).unwrap();
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn seated(lobby: &Value) -> Vec<(u64, String)> {
    lobby["seated"]
        .as_array()
        .unwrap()
        .iter()
        .map(|entry| {
            (
                entry["slot"].as_u64().unwrap(),
                entry["ember_id"].as_str().unwrap().to_owned(),
            )
        })
        .collect()
}

pub fn queued(lobby: &Value) -> Vec<String> {
    lobby["queue"]
        .as_array()
        .unwrap()
        .iter()
        .map(|entry| entry["ember_id"].as_str().unwrap().to_owned())
        .collect()
}

pub fn last<'a>(events: &'a [serde_json::Value], kind: &str) -> &'a Value {
    events
        .iter()
        .rev()
        .find(|event| event["type"] == format!("io.ember.tournament.{kind}.v1"))
        .unwrap_or_else(|| panic!("no {kind} event"))
}
