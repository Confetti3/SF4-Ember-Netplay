//! Lobbies: an opt-in queue that plays first-to-N sets one after another and
//! rotates its seats when a set ends (an extension beyond EMBER-TB-001).
mod common;

use common::{Bridge, Player, code, types};
use reqwest::StatusCode;
use serde_json::{Value, json};

struct Fixture {
    bridge: Bridge,
    provider: String,
    organizer: String,
    players: Vec<Player>,
    links: Vec<Value>,
}

async fn fixture(count: u8) -> Fixture {
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
    fn id(&self, index: usize) -> &str {
        self.players[index].id().as_str()
    }

    fn participant(&self, index: usize) -> &str {
        self.links[index]["participant_id"].as_str().unwrap()
    }

    async fn create(&self, external: &str, games_to_win: u8, rotation: &str) -> Value {
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

    async fn join(&self, lobby: &str, index: usize) -> (StatusCode, Value) {
        self.bridge
            .post_keyed(
                &self.provider,
                &format!("/v1/lobbies/{lobby}/queue"),
                json!({ "participant_id": self.participant(index), "ember_id": self.id(index) }),
                Some(&format!("join-{lobby}-{index}-{}", rand_key())),
            )
            .await
    }

    async fn lobby(&self, lobby: &str) -> Value {
        let (status, body) = self
            .bridge
            .get(&self.provider, &format!("/v1/lobbies/{lobby}"))
            .await;
        assert_eq!(status, StatusCode::OK, "{body}");
        body
    }

    /// The organizer records one game of the lobby's running set.
    async fn win(&self, lobby: &str, winner_slot: u8) -> (StatusCode, Value) {
        let current = self.lobby(lobby).await;
        let id = current["current_match_id"]
            .as_str()
            .expect("a running set")
            .to_owned();
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
}

fn rand_key() -> String {
    let mut bytes = [0u8; 8];
    getrandom::fill(&mut bytes).unwrap();
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn seated(lobby: &Value) -> Vec<(u64, String)> {
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

fn queued(lobby: &Value) -> Vec<String> {
    lobby["queue"]
        .as_array()
        .unwrap()
        .iter()
        .map(|entry| entry["ember_id"].as_str().unwrap().to_owned())
        .collect()
}

fn last<'a>(events: &'a [serde_json::Value], kind: &str) -> &'a Value {
    events
        .iter()
        .rev()
        .find(|event| event["type"] == format!("io.ember.tournament.{kind}.v1"))
        .unwrap_or_else(|| panic!("no {kind} event"))
}

// King of the hill: the winner keeps the seat, the loser rejoins the back of
// the queue, and only players who joined are ever seated.
#[tokio::test]
async fn winner_stays_rotates_the_queue() {
    let f = fixture(4).await;
    let (status, caps) = f.bridge.get(&f.provider, "/v1/capabilities").await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(
        caps["lobby_rotations"],
        json!(["winner_stays", "loser_stays", "both_rotate"])
    );
    let lobby = f.create("hill", 2, "winner_stays").await;
    let id = lobby["lobby_id"].as_str().unwrap().to_owned();
    assert_eq!(lobby["state"], "open");
    for index in 0..3 {
        let (status, body) = f.join(&id, index).await;
        assert_eq!(status, StatusCode::CREATED, "{body}");
    }
    // Joining again while queued or seated changes nothing.
    let (status, _) = f.join(&id, 2).await;
    assert_eq!(status, StatusCode::OK);
    let current = f.lobby(&id).await;
    assert_eq!(
        seated(&current),
        vec![(0, f.id(0).into()), (1, f.id(1).into())]
    );
    assert_eq!(queued(&current), vec![f.id(2).to_owned()]);
    let first_set = current["current_match_id"].as_str().unwrap().to_owned();

    for slot in [0, 1, 0] {
        let (status, body) = f.win(&id, slot).await;
        assert_eq!(status, StatusCode::CREATED, "{body}");
    }
    let (_, finished) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{first_set}"))
        .await;
    assert_eq!(finished["state"], "completed");
    let after = f.lobby(&id).await;
    assert_eq!(
        seated(&after),
        vec![(0, f.id(0).into()), (1, f.id(2).into())]
    );
    assert_eq!(queued(&after), vec![f.id(1).to_owned()]);
    assert_eq!(after["streak"], json!({ "ember_id": f.id(0), "sets": 1 }));
    assert_eq!(after["sets_completed"], 1);
    assert_ne!(after["current_match_id"].as_str().unwrap(), first_set);

    let events = f.bridge.events(&f.provider, "0").await;
    let completed = last(&events, "lobby.set.completed");
    assert_eq!(completed["data"]["winner_id"], f.id(0));
    assert_eq!(completed["data"]["loser_id"], f.id(1));
    assert_eq!(completed["data"]["queue"], json!([f.id(1)]));
    assert_eq!(completed["data"]["streak"]["sets"], 1);
    let started = last(&events, "lobby.set.started");
    assert_eq!(started["data"]["set"], 2);
    assert_eq!(started["data"]["participants"][1]["ember_id"], f.id(2));
    let kinds = types(&events);
    let at = |kind: &str| {
        kinds
            .iter()
            .rposition(|k| k == &format!("io.ember.tournament.{kind}.v1"))
            .unwrap()
    };
    assert!(at("match.completed") < at("lobby.set.completed"));
    assert!(at("lobby.set.completed") < at("lobby.set.started"));

    // The king wins again: the challenger goes to the back.
    f.win(&id, 0).await;
    f.win(&id, 0).await;
    let after = f.lobby(&id).await;
    assert_eq!(
        seated(&after),
        vec![(0, f.id(0).into()), (1, f.id(1).into())]
    );
    assert_eq!(queued(&after), vec![f.id(2).to_owned()]);
    assert_eq!(after["streak"]["sets"], 2);
    // A linked player who never joined is never given a seat.
    assert!(!queued(&after).contains(&f.id(3).to_owned()));

    // Players who joined read the lobby and its events; one who did not cannot.
    let (status, _) = f
        .bridge
        .get(f.players[1].token(), &format!("/v1/lobbies/{id}"))
        .await;
    assert_eq!(status, StatusCode::OK);
    let (status, _) = f
        .bridge
        .get(f.players[3].token(), &format!("/v1/lobbies/{id}"))
        .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let theirs = types(&f.bridge.events(f.players[2].token(), "0").await);
    assert!(theirs.contains(&"io.ember.tournament.lobby.queue.changed.v1".to_owned()));
    let outsider = types(&f.bridge.events(f.players[3].token(), "0").await);
    assert!(!outsider.iter().any(|kind| kind.contains(".lobby.")));
}

#[tokio::test]
async fn loser_stays_and_both_rotate() {
    let f = fixture(4).await;
    let lobby = f.create("loser", 1, "loser_stays").await;
    let id = lobby["lobby_id"].as_str().unwrap().to_owned();
    for index in 0..3 {
        f.join(&id, index).await;
    }
    f.win(&id, 0).await;
    let after = f.lobby(&id).await;
    assert_eq!(
        seated(&after),
        vec![(0, f.id(2).into()), (1, f.id(1).into())]
    );
    assert_eq!(queued(&after), vec![f.id(0).to_owned()]);
    // The winner left the seat, so there is no streak to carry.
    assert_eq!(after["streak"], Value::Null);

    let lobby = f.create("both", 1, "both_rotate").await;
    let both = lobby["lobby_id"].as_str().unwrap().to_owned();
    // Players can be in one open lobby at a time on a connection.
    let (status, refused) = f.join(&both, 0).await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "lease_conflict")
    );
    let (status, closed) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/lobbies/{id}/close"),
            json!({ "reason": "Moving to the next lobby", "expected_revision": f.lobby(&id).await["revision"] }),
            Some("close-loser"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{closed}");
    assert_eq!(closed["state"], "closed");
    for index in 0..4 {
        let (status, body) = f.join(&both, index).await;
        assert_eq!(status, StatusCode::CREATED, "{body}");
    }
    f.win(&both, 1).await;
    let after = f.lobby(&both).await;
    assert_eq!(
        seated(&after),
        vec![(0, f.id(2).into()), (1, f.id(3).into())]
    );
    // Winner first, then loser.
    assert_eq!(queued(&after), vec![f.id(1).to_owned(), f.id(0).to_owned()]);
}

// Leaving a seat or losing the link cancels the running set; the other player
// stays and the next queued player sits down. Lobby sets cannot be cancelled
// or reopened directly, and their IDs are reserved.
#[tokio::test]
async fn leaving_unlinking_and_closing() {
    let f = fixture(3).await;
    let lobby = f.create("night", 2, "winner_stays").await;
    let id = lobby["lobby_id"].as_str().unwrap().to_owned();
    for index in 0..3 {
        f.join(&id, index).await;
    }
    let first_set = f.lobby(&id).await["current_match_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (status, refused) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/matches/{first_set}/cancel"),
            json!({ "reason": "test", "expected_revision": "1" }),
            Some("cancel-lobby-set"),
        )
        .await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "lease_conflict")
    );
    let (status, refused) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/matches",
            json!({
                "external_match_id": format!("lobby:{id}:9"),
                "game": "usf4",
                "participants": [
                    { "participant_id": f.participant(0), "ember_id": f.id(0), "slot": 0 },
                    { "participant_id": f.participant(1), "ember_id": f.id(1), "slot": 1 },
                ],
                "rules": {
                    "games_to_win": 2, "draw_policy": "replay_no_score", "native_rules_profile": "organizer-reported-v1",
                    "edition_policy": "ultra_only", "character_policy": "unrestricted_between_games",
                    "stage_policy": "p1_selects", "input_delay_policy": "ember_existing_ready_policy",
                },
                "observer_policy": "authorized_only",
                "result_policy": "two_player_agreement_or_review",
                "required_build_id": "test-build",
                "metadata": {},
            }),
            Some("reserved-id"),
        )
        .await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::BAD_REQUEST, "invalid_request")
    );

    // The seated player in slot 1 leaves: their set is cancelled.
    let (status, after) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/lobbies/{id}/queue/{}/leave", f.participant(1)),
            json!({}),
            Some("leave-1"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{after}");
    let (_, cancelled) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{first_set}"))
        .await;
    assert_eq!(cancelled["state"], "cancelled");
    assert_eq!(
        seated(&after),
        vec![(0, f.id(0).into()), (1, f.id(2).into())]
    );
    assert!(queued(&after).is_empty());

    // The second set is won, then its deciding game cannot be voided because
    // the lobby has already seated the next set.
    f.join(&id, 1).await;
    f.win(&id, 0).await;
    let (status, done) = f.win(&id, 0).await;
    assert_eq!(status, StatusCode::CREATED, "{done}");
    let events = f.bridge.events(&f.organizer, "0").await;
    let completed = last(&events, "lobby.set.completed");
    let second_set = completed["data"]["match_id"].as_str().unwrap().to_owned();
    let (_, found) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{second_set}"))
        .await;
    let attempt = found["attempts"][1]["attempt_id"].as_str().unwrap();
    let (status, refused) = f
        .bridge
        .post_keyed(
            &f.organizer,
            &format!("/v1/matches/{second_set}/adjudications"),
            json!({ "kind": "void_game", "attempt_id": attempt, "reason": "Wrong winner", "expected_revision": found["revision"] }),
            Some("void-lobby-set"),
        )
        .await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "lease_conflict")
    );

    // Unlinking a seated player cancels their set the same way.
    let after = f.lobby(&id).await;
    assert_eq!(
        seated(&after),
        vec![(0, f.id(0).into()), (1, f.id(1).into())]
    );
    let third_set = after["current_match_id"].as_str().unwrap().to_owned();
    let link = f.links[1]["link_id"].as_str().unwrap();
    let removed = f
        .bridge
        .client
        .delete(f.bridge.url(&format!("/v1/links/{link}")))
        .bearer_auth(&f.provider)
        .send()
        .await
        .unwrap();
    assert_eq!(removed.status(), StatusCode::OK);
    let (_, cancelled) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{third_set}"))
        .await;
    assert_eq!(cancelled["state"], "cancelled");
    let after = f.lobby(&id).await;
    assert_eq!(
        seated(&after),
        vec![(0, f.id(0).into()), (1, f.id(2).into())]
    );
    let events = f.bridge.events(&f.provider, "0").await;
    assert_eq!(
        last(&events, "lobby.queue.changed")["data"]["reason"],
        "identity_unlinked"
    );

    // Closing cancels the running set and empties the lobby.
    let running = after["current_match_id"].as_str().unwrap().to_owned();
    let (status, closed) = f
        .bridge
        .post_keyed(
            &f.organizer,
            &format!("/v1/lobbies/{id}/close"),
            json!({ "reason": "End of stream", "expected_revision": after["revision"] }),
            Some("close-night"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{closed}");
    assert!(seated(&closed).is_empty() && queued(&closed).is_empty());
    let (_, cancelled) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{running}"))
        .await;
    assert_eq!(cancelled["state"], "cancelled");
    let (status, refused) = f.join(&id, 0).await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "stale_revision")
    );
}

// A queued player with another active match on the connection keeps their
// place and is skipped until it ends.
#[tokio::test]
async fn a_busy_player_keeps_their_place() {
    let f = fixture(5).await;
    let (status, bracket) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/matches",
            json!({
                "external_match_id": "bracket-1",
                "game": "usf4",
                "participants": [
                    { "participant_id": f.participant(2), "ember_id": f.id(2), "slot": 0 },
                    { "participant_id": f.participant(3), "ember_id": f.id(3), "slot": 1 },
                ],
                "rules": {
                    "games_to_win": 1, "draw_policy": "replay_no_score", "native_rules_profile": "organizer-reported-v1",
                    "edition_policy": "ultra_only", "character_policy": "unrestricted_between_games",
                    "stage_policy": "p1_selects", "input_delay_policy": "ember_existing_ready_policy",
                },
                "observer_policy": "authorized_only",
                "result_policy": "two_player_agreement_or_review",
                "required_build_id": "test-build",
                "metadata": {},
            }),
            Some("bracket-1"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{bracket}");
    let lobby = f.create("busy", 1, "winner_stays").await;
    let id = lobby["lobby_id"].as_str().unwrap().to_owned();
    for index in [0, 1, 2, 4] {
        f.join(&id, index).await;
    }
    f.win(&id, 0).await;
    let after = f.lobby(&id).await;
    assert_eq!(
        seated(&after),
        vec![(0, f.id(0).into()), (1, f.id(4).into())]
    );
    assert_eq!(queued(&after), vec![f.id(2).to_owned(), f.id(1).to_owned()]);
}
