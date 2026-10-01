//! Tournaments: brackets the bridge runs on a provider connection (an
//! extension beyond EMBER-TB-001). Each set is an ordinary match.
mod common;

use common::{
    code,
    league::{Fixture, fixture, rand_key},
    types,
};
use reqwest::StatusCode;
use serde_json::{Value, json};

async fn create(f: &Fixture, external: &str, body: Value) -> String {
    let mut command = json!({
        "external_tournament_id": external,
        "game": "usf4",
        "games_to_win": 1,
        "required_build_id": "test-build",
        "metadata": { "title": "Friday night" },
    });
    for (key, value) in body.as_object().unwrap() {
        command[key] = value.clone();
    }
    let (status, created) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/tournaments",
            command,
            Some(&format!("create-{external}")),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    assert_eq!(created["state"], "registration");
    created["tournament_id"].as_str().unwrap().to_owned()
}

async fn register(f: &Fixture, id: &str, index: usize) -> (StatusCode, Value) {
    f.bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/tournaments/{id}/entrants"),
            json!({ "participant_id": f.participant(index), "ember_id": f.id(index) }),
            Some(&format!("register-{id}-{index}-{}", rand_key())),
        )
        .await
}

async fn tournament(f: &Fixture, id: &str) -> Value {
    let (status, body) = f
        .bridge
        .get(&f.provider, &format!("/v1/tournaments/{id}"))
        .await;
    assert_eq!(status, StatusCode::OK, "{body}");
    body
}

async fn start(f: &Fixture, id: &str, seeding: Option<Vec<usize>>) -> Value {
    let current = tournament(f, id).await;
    let mut body = json!({ "expected_revision": current["revision"] });
    if let Some(order) = seeding {
        body["seeding"] = json!(
            order
                .iter()
                .map(|&index| f.participant(index))
                .collect::<Vec<_>>()
        );
    }
    let (status, started) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/tournaments/{id}/start"),
            body,
            Some(&format!("start-{id}")),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{started}");
    started
}

/// Which fixture player an entry in a set's `players` is.
fn index_of(f: &Fixture, player: &Value) -> usize {
    (0..f.players.len())
        .find(|&index| player["ember_id"] == f.id(index))
        .expect("a fixture player")
}

/// Plays every running set, the winner chosen by `pick` from the two fixture
/// indexes, until none is left. Returns the finished snapshot.
async fn play_out(f: &Fixture, id: &str, pick: impl Fn(&Value, [usize; 2]) -> u8) -> Value {
    for _ in 0..64 {
        let current = tournament(f, id).await;
        let playing: Vec<Value> = current["sets"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|set| set["status"] == "playing")
            .cloned()
            .collect();
        if playing.is_empty() {
            return current;
        }
        for set in playing {
            let players = [
                index_of(f, &set["players"][0]),
                index_of(f, &set["players"][1]),
            ];
            let winner = pick(&set, players);
            let match_id = set["match_id"].as_str().unwrap();
            for _ in 0..set["games_to_win"].as_u64().unwrap() {
                let (status, body) = f.game(match_id, winner).await;
                assert_eq!(status, StatusCode::CREATED, "{body}");
            }
        }
    }
    panic!("the tournament did not finish");
}

/// The lower fixture index (the better seed in registration order) wins.
fn better_wins(_: &Value, players: [usize; 2]) -> u8 {
    u8::from(players[1] < players[0])
}

fn placement(snapshot: &Value, f: &Fixture, index: usize) -> u64 {
    snapshot["entrants"]
        .as_array()
        .unwrap()
        .iter()
        .find(|entrant| entrant["ember_id"] == f.id(index))
        .and_then(|entrant| entrant["placement"].as_u64())
        .expect("a placement")
}

#[tokio::test]
async fn single_elimination_runs_to_placements() {
    let f = fixture(5).await;
    let id = create(&f, "single", json!({ "format": "single_elimination" })).await;
    for index in 0..5 {
        let (status, body) = register(&f, &id, index).await;
        assert_eq!(status, StatusCode::CREATED, "{body}");
    }
    // Registering again changes nothing.
    assert_eq!(register(&f, &id, 0).await.0, StatusCode::OK);
    let started = start(&f, &id, None).await;
    assert_eq!(started["state"], "running");
    let sets = started["sets"].as_array().unwrap();
    // Eight slots for five players: the top three seeds have byes.
    assert_eq!(sets.iter().filter(|set| set["status"] == "bye").count(), 3);
    let first = sets.iter().find(|set| set["status"] == "playing").unwrap();
    let (_, created) = f
        .bridge
        .get(
            &f.organizer,
            &format!("/v1/matches/{}", first["match_id"].as_str().unwrap()),
        )
        .await;
    assert_eq!(created["metadata"]["round_label"], first["label"]);
    assert_eq!(created["metadata"]["title"], "Friday night");

    let done = play_out(&f, &id, better_wins).await;
    assert_eq!(done["state"], "completed");
    assert_eq!(
        (0..5)
            .map(|index| placement(&done, &f, index))
            .collect::<Vec<_>>(),
        vec![1, 2, 3, 3, 5]
    );
    // Registration has closed.
    assert_eq!(register(&f, &id, 0).await.0, StatusCode::CONFLICT);

    let kinds = types(&f.bridge.events(&f.provider, "0").await);
    for kind in [
        "tournament.created",
        "tournament.started",
        "tournament.match.started",
        "tournament.match.completed",
        "tournament.completed",
    ] {
        assert!(
            kinds
                .iter()
                .any(|seen| seen.ends_with(&format!("{kind}.v1"))),
            "{kind}"
        );
    }
    // Entrants read the tournament and its events with their own session.
    let theirs = types(&f.bridge.events(f.players[4].token(), "0").await);
    assert!(
        theirs
            .iter()
            .any(|seen| seen.ends_with("tournament.completed.v1"))
    );
    let (status, seen) = f
        .bridge
        .get(f.players[4].token(), &format!("/v1/tournaments/{id}"))
        .await;
    assert_eq!(status, StatusCode::OK);
    // A player sees their own account handle and nobody else's, in the
    // snapshot or in events.
    for entrant in seen["entrants"].as_array().unwrap() {
        assert_eq!(
            entrant["participant_id"].is_string(),
            entrant["ember_id"] == f.id(4),
            "{entrant}"
        );
    }
    assert!(!seen.to_string().contains(f.participant(0)));
    let started_event = f
        .bridge
        .events(f.players[4].token(), "0")
        .await
        .into_iter()
        .find(|event| {
            event["type"]
                .as_str()
                .unwrap()
                .ends_with("tournament.started.v1")
        })
        .unwrap();
    assert!(!started_event.to_string().contains(f.participant(0)));
    // A record counts tournament sets like any other.
    let (_, record) = f
        .bridge
        .get(&f.provider, &format!("/v1/players/{}/record", f.id(0)))
        .await;
    assert_eq!(record["sets"]["won"], 2);
    assert_eq!(record["recent"][0]["tournament_id"], id.as_str());
}

#[tokio::test]
async fn double_elimination_resets_and_plays_longer_finals() {
    let f = fixture(4).await;
    let id = create(
        &f,
        "double",
        json!({ "format": "double_elimination", "finals_games_to_win": 2 }),
    )
    .await;
    for index in 0..4 {
        register(&f, &id, index).await;
    }
    // Seed in reverse: player 3 is the top seed.
    start(&f, &id, Some(vec![3, 2, 1, 0])).await;
    let done = play_out(&f, &id, |set, players| {
        // The losers-bracket player takes the first grand final, forcing the reset.
        if set["label"] == "Grand final" {
            1
        } else {
            u8::from(players[1] > players[0])
        }
    })
    .await;
    assert_eq!(done["state"], "completed");
    let sets = done["sets"].as_array().unwrap();
    let grand_final = sets
        .iter()
        .find(|set| set["label"] == "Grand final")
        .unwrap();
    let reset = sets
        .iter()
        .find(|set| set["label"] == "Grand final reset")
        .unwrap();
    assert_eq!(grand_final["games_to_win"], 2);
    assert_eq!(reset["status"], "played");
    let mut places: Vec<u64> = (0..4).map(|index| placement(&done, &f, index)).collect();
    places.sort();
    assert_eq!(places, vec![1, 2, 3, 4]);
    // The top seed won every set but the first grand final, then the reset.
    assert_eq!(placement(&done, &f, 3), 1);
}

#[tokio::test]
async fn round_robin_ranks_by_sets_then_games() {
    let f = fixture(4).await;
    let id = create(
        &f,
        "robin",
        json!({ "format": "round_robin", "games_to_win": 2 }),
    )
    .await;
    for index in 0..4 {
        register(&f, &id, index).await;
    }
    let started = start(&f, &id, None).await;
    assert_eq!(started["sets"].as_array().unwrap().len(), 6);
    let done = play_out(&f, &id, better_wins).await;
    assert_eq!(done["state"], "completed");
    let standings = done["standings"].as_array().unwrap();
    let order: Vec<usize> = standings.iter().map(|row| index_of(&f, row)).collect();
    assert_eq!(order, vec![0, 1, 2, 3]);
    assert_eq!(standings[0]["sets_won"], 3);
    assert_eq!(standings[0]["games_won"], 6);
    assert_eq!(placement(&done, &f, 3), 4);
}

// A set whose player is busy elsewhere waits and starts when that match ends,
// and a lobby leaves a player whose bracket set is ready alone.
#[tokio::test]
async fn a_bracket_set_waits_and_comes_before_a_lobby() {
    let f = fixture(4).await;
    let id = create(&f, "busy", json!({ "format": "single_elimination" })).await;
    register(&f, &id, 0).await;
    register(&f, &id, 1).await;
    let side = f.bracket("side-match", 0, 2).await;
    let started = start(&f, &id, None).await;
    let final_set = &started["sets"][0];
    assert_eq!(final_set["status"], "ready");
    assert!(final_set["match_id"].is_null());

    let lobby = f.create("hill", 1, "winner_stays").await;
    let lobby_id = lobby["lobby_id"].as_str().unwrap().to_owned();
    f.join(&lobby_id, 1).await;
    f.join(&lobby_id, 3).await;
    let waiting = f.lobby(&lobby_id).await;
    assert!(waiting["current_match_id"].is_null(), "{waiting}");
    assert!(
        waiting["seated"]
            .as_array()
            .unwrap()
            .iter()
            .all(|seat| seat["ember_id"] != f.id(1)),
        "{waiting}"
    );

    // The side match ends: the bracket set starts, and the lobby still waits.
    let (status, body) = f.game(side["match_id"].as_str().unwrap(), 0).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    let now = tournament(&f, &id).await;
    assert_eq!(now["sets"][0]["status"], "playing");
    assert!(f.lobby(&lobby_id).await["current_match_id"].is_null());

    // Once the bracket set ends the lobby can seat player 1.
    let (status, body) = f
        .game(now["sets"][0]["match_id"].as_str().unwrap(), 1)
        .await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    assert_eq!(tournament(&f, &id).await["state"], "completed");
    assert!(f.lobby(&lobby_id).await["current_match_id"].is_string());
}

#[tokio::test]
async fn a_withdrawal_hands_the_set_over() {
    let f = fixture(4).await;
    let id = create(&f, "withdraw", json!({ "format": "single_elimination" })).await;
    for index in 0..4 {
        register(&f, &id, index).await;
    }
    let started = start(&f, &id, None).await;
    let semifinal = started["sets"]
        .as_array()
        .unwrap()
        .iter()
        .find(|set| set["players"][0]["ember_id"] == f.id(0))
        .unwrap()
        .clone();
    assert_eq!(semifinal["status"], "playing");
    // A tournament set cannot be cancelled directly.
    let match_id = semifinal["match_id"].as_str().unwrap();
    let (status, refused) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/matches/{match_id}/cancel"),
            json!({ "reason": "test", "expected_revision": "1" }),
            Some("cancel-bracket-set"),
        )
        .await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "lease_conflict")
    );

    let (status, body) = f
        .bridge
        .post_keyed(
            &f.organizer,
            &format!(
                "/v1/tournaments/{id}/entrants/{}/withdraw",
                f.participant(0)
            ),
            json!({}),
            Some("withdraw-0"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{body}");
    let (_, cancelled) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{match_id}"))
        .await;
    assert_eq!(cancelled["state"], "cancelled");
    let after = tournament(&f, &id).await;
    let walkover = after["sets"]
        .as_array()
        .unwrap()
        .iter()
        .find(|set| set["node"] == semifinal["node"])
        .unwrap();
    assert_eq!(walkover["status"], "walkover");
    let done = play_out(&f, &id, better_wins).await;
    assert_eq!(done["state"], "completed");
    assert_eq!(placement(&done, &f, 0), 3);
    assert_eq!(placement(&done, &f, 1), 1);
}

#[tokio::test]
async fn a_correction_reopens_the_bracket_until_a_later_game() {
    let f = fixture(4).await;
    let id = create(&f, "correct", json!({ "format": "single_elimination" })).await;
    for index in 0..4 {
        register(&f, &id, index).await;
    }
    let started = start(&f, &id, None).await;
    let sets = started["sets"].as_array().unwrap().clone();
    let first = sets
        .iter()
        .find(|set| set["players"][0]["ember_id"] == f.id(0))
        .unwrap();
    let second = sets
        .iter()
        .find(|set| set["players"][0]["ember_id"] == f.id(1))
        .unwrap();
    let first_match = first["match_id"].as_str().unwrap().to_owned();
    // The wrong slot is recorded for the first semifinal, then voided.
    f.game(&first_match, 0).await;
    let (_, snapshot) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{first_match}"))
        .await;
    let attempt = snapshot["attempts"][0]["attempt_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (status, body) = f.void(&first_match, &attempt).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    let reopened = tournament(&f, &id).await;
    let final_set = reopened["sets"].as_array().unwrap().last().unwrap().clone();
    assert!(final_set["players"][0].is_null(), "{final_set}");
    f.game(&first_match, 1).await;

    // The other semifinal and a game of the final are played: now the first
    // semifinal's result is final.
    f.game(second["match_id"].as_str().unwrap(), 0).await;
    let now = tournament(&f, &id).await;
    let final_set = now["sets"].as_array().unwrap().last().unwrap().clone();
    assert_eq!(final_set["status"], "playing");
    assert_eq!(index_of(&f, &final_set["players"][0]), 3);
    // Before the final has a game, reopening the semifinal cancels its match.
    let (_, snapshot) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{first_match}"))
        .await;
    let accepted = snapshot["attempts"]
        .as_array()
        .unwrap()
        .iter()
        .find(|attempt| attempt["state"] == "accepted")
        .unwrap()["attempt_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (status, body) = f.void(&first_match, &accepted).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    let (_, final_match) = f
        .bridge
        .get(
            &f.organizer,
            &format!("/v1/matches/{}", final_set["match_id"].as_str().unwrap()),
        )
        .await;
    assert_eq!(final_match["state"], "cancelled");
    f.game(&first_match, 1).await;
    let now = tournament(&f, &id).await;
    let final_set = now["sets"].as_array().unwrap().last().unwrap().clone();
    let final_match = final_set["match_id"].as_str().unwrap().to_owned();
    f.game(&final_match, 0).await;
    // The final is first to 1, so the tournament is over and the
    // semifinal's result can no longer be corrected.
    let (_, snapshot) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{first_match}"))
        .await;
    let accepted = snapshot["attempts"]
        .as_array()
        .unwrap()
        .iter()
        .find(|attempt| attempt["state"] == "accepted")
        .unwrap()["attempt_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (status, refused) = f.void(&first_match, &accepted).await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "lease_conflict"),
        "{refused}"
    );
}

#[tokio::test]
async fn a_later_game_blocks_a_correction() {
    let f = fixture(4).await;
    let id = create(
        &f,
        "blocked",
        json!({ "format": "single_elimination", "finals_games_to_win": 2 }),
    )
    .await;
    for index in 0..4 {
        register(&f, &id, index).await;
    }
    let started = start(&f, &id, None).await;
    for set in started["sets"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|set| set["status"] == "playing")
    {
        f.game(set["match_id"].as_str().unwrap(), 0).await;
    }
    let now = tournament(&f, &id).await;
    let final_set = now["sets"].as_array().unwrap().last().unwrap().clone();
    f.game(final_set["match_id"].as_str().unwrap(), 0).await;
    let semifinal = now["sets"][0]["match_id"].as_str().unwrap().to_owned();
    let (_, snapshot) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{semifinal}"))
        .await;
    let attempt = snapshot["attempts"][0]["attempt_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (status, refused) = f.void(&semifinal, &attempt).await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "lease_conflict"),
        "{refused}"
    );
    // Nothing changed.
    let (_, unchanged) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{semifinal}"))
        .await;
    assert_eq!(unchanged["state"], "completed");

    // Cancelling the tournament cancels the final's match.
    let current = tournament(&f, &id).await;
    let (status, body) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/tournaments/{id}/cancel"),
            json!({ "reason": "Venue closed", "expected_revision": current["revision"] }),
            Some("cancel-blocked"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{body}");
    assert_eq!(body["state"], "cancelled");
    let (_, final_match) = f
        .bridge
        .get(
            &f.organizer,
            &format!("/v1/matches/{}", final_set["match_id"].as_str().unwrap()),
        )
        .await;
    assert_eq!(final_match["state"], "cancelled");
}

// An ended link withdraws the player: their running set goes to the opponent.
#[tokio::test]
async fn unlinking_withdraws_an_entrant() {
    let f = fixture(2).await;
    let id = create(&f, "unlink", json!({ "format": "double_elimination" })).await;
    register(&f, &id, 0).await;
    register(&f, &id, 1).await;
    let started = start(&f, &id, None).await;
    let first = started["sets"][0]["match_id"].as_str().unwrap().to_owned();
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
        .get(&f.organizer, &format!("/v1/matches/{first}"))
        .await;
    assert_eq!(cancelled["state"], "cancelled");
    let done = tournament(&f, &id).await;
    assert_eq!(done["state"], "completed", "{done}");
    assert_eq!(placement(&done, &f, 0), 1);
    assert_eq!(placement(&done, &f, 1), 2);
    let withdrawn = done["entrants"]
        .as_array()
        .unwrap()
        .iter()
        .find(|entrant| entrant["ember_id"] == f.id(1))
        .unwrap();
    assert_eq!(withdrawn["state"], "withdrawn");
    // The champion took the grand final by walkover; nothing says they play the reset.
    let grand_final = f
        .bridge
        .events(&f.provider, "0")
        .await
        .into_iter()
        .find(|event| {
            event["type"]
                .as_str()
                .unwrap()
                .ends_with("tournament.match.completed.v1")
                && event["data"]["label"] == "Grand final"
        })
        .unwrap();
    assert!(
        grand_final["data"]["winner_next"].is_null(),
        "{grand_final}"
    );
    // The winners-bracket player took the grand final, so there is no reset.
    let reset = done["sets"]
        .as_array()
        .unwrap()
        .iter()
        .find(|set| set["label"] == "Grand final reset")
        .unwrap();
    assert_eq!(reset["status"], "skipped");
}

// Once a tournament is over its results are final, even for a correction
// that would leave the set finished.
#[tokio::test]
async fn a_finished_tournament_is_final() {
    let f = fixture(2).await;
    let id = create(
        &f,
        "final",
        json!({ "format": "single_elimination", "games_to_win": 2 }),
    )
    .await;
    register(&f, &id, 0).await;
    register(&f, &id, 1).await;
    let started = start(&f, &id, None).await;
    let match_id = started["sets"][0]["match_id"].as_str().unwrap().to_owned();
    for winner in [0, 1, 0] {
        f.game(&match_id, winner).await;
    }
    assert_eq!(tournament(&f, &id).await["state"], "completed");
    let (_, snapshot) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{match_id}"))
        .await;
    // Voiding the loser's game would keep the set at 2-0.
    let attempt = snapshot["attempts"][1]["attempt_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (status, refused) = f.void(&match_id, &attempt).await;
    assert_eq!(
        (status, code(&refused)),
        (StatusCode::CONFLICT, "lease_conflict"),
        "{refused}"
    );
}

// Cancelling a tournament frees the players a ready set held back from a lobby.
#[tokio::test]
async fn cancelling_frees_players_for_lobbies() {
    let f = fixture(4).await;
    let id = create(
        &f,
        "cancel-lobby",
        json!({ "format": "single_elimination" }),
    )
    .await;
    register(&f, &id, 0).await;
    register(&f, &id, 1).await;
    f.bracket("cancel-side", 0, 2).await;
    start(&f, &id, None).await;
    let lobby = f.create("cancel-hill", 1, "winner_stays").await;
    let lobby_id = lobby["lobby_id"].as_str().unwrap().to_owned();
    f.join(&lobby_id, 1).await;
    f.join(&lobby_id, 3).await;
    assert!(f.lobby(&lobby_id).await["current_match_id"].is_null());
    let current = tournament(&f, &id).await;
    let (status, body) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/tournaments/{id}/cancel"),
            json!({ "reason": "Called off", "expected_revision": current["revision"] }),
            Some("cancel-lobby-tournament"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{body}");
    assert!(f.lobby(&lobby_id).await["current_match_id"].is_string());
}
