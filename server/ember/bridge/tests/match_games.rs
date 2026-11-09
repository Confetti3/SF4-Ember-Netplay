//! Organised matches for each game (migration 059): a match, lobby or
//! tournament names `usf4` or `sfv`, its sets take its game, a player's
//! assignments say each match's game, and a record counts one game's
//! matches, USF4's when the request names none.
mod common;

use common::{
    code,
    league::{Fixture, fixture, rand_key},
};
use reqwest::StatusCode;
use serde_json::{Value, json};

/// The raw body of `GET /v1/players/{player}/record`, with `query`, as the
/// provider reads it.
async fn record_bytes(f: &Fixture, player: usize, query: &str) -> Vec<u8> {
    let response = f
        .bridge
        .client
        .get(
            f.bridge
                .url(&format!("/v1/players/{}/record{query}", f.id(player))),
        )
        .bearer_auth(&f.provider)
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    response.bytes().await.unwrap().to_vec()
}

/// A player's assignments, with `query`.
async fn assignments(f: &Fixture, player: usize, query: &str) -> Vec<Value> {
    let (status, listed) = f
        .bridge
        .get(
            f.players[player].token(),
            &format!("/v1/assignments{query}"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{listed}");
    listed["assignments"].as_array().unwrap().clone()
}

fn game_of(listed: &[Value], match_id: &Value) -> Value {
    listed
        .iter()
        .find(|row| row["match_id"] == *match_id)
        .unwrap_or_else(|| panic!("{match_id} is not listed"))["game"]
        .clone()
}

#[tokio::test]
async fn the_bridge_offers_both_games_and_checks_usf4_as_before() {
    let f = fixture(2).await;
    let (status, caps) = f.bridge.get(&f.provider, "/v1/capabilities").await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(caps["games"], json!(["usf4", "sfv"]));

    // USF4's sets are Ultra only; another game, or none, is refused as before.
    for (game, edition) in [
        ("usf4", Some("not_applicable")),
        ("usf4", None),
        ("sf6", Some("ultra_only")),
        ("", Some("ultra_only")),
    ] {
        let external = format!("refused-{game}-{edition:?}");
        let command = f.match_command(game, edition, &external, [0, 1], 2);
        let (status, body) = f
            .bridge
            .post_keyed(&f.provider, "/v1/matches", command, Some(&external))
            .await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{body}");
        assert_eq!(code(&body), "invalid_request", "{body}");
    }
    // SFV has no edition: its sets may leave it out, and whatever they name
    // is not read.
    let sfv = f
        .ordinary_in("sfv", None, "sfv-no-edition", [0, 1], 1)
        .await;
    let (status, body) = f.game(sfv["match_id"].as_str().unwrap(), 0).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    f.ordinary_in("sfv", Some("ultra_only"), "sfv-any-edition", [0, 1], 1)
        .await;
}

#[tokio::test]
async fn assignments_are_one_games_chosen_before_the_limit() {
    let f = fixture(3).await;
    let usf4 = f.bracket("usf4-set", 0, 1).await;
    // Decided, so player 0 is free for the next; it stays listed.
    let (status, body) = f.game(usf4["match_id"].as_str().unwrap(), 0).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    // More newer SFV sets than the list holds.
    let mut sfv = Vec::new();
    for set in 0..51 {
        let created = f
            .ordinary_in("sfv", None, &format!("sfv-set-{set}"), [0, 2], 1)
            .await;
        let (status, body) = f.game(created["match_id"].as_str().unwrap(), 1).await;
        assert_eq!(status, StatusCode::CREATED, "{body}");
        sfv.push(created["match_id"].clone());
    }
    // Without a game the list is USF4's, as every list was before games, and
    // SFV's history does not push the older USF4 set out of it.
    let listed = assignments(&f, 0, "").await;
    assert_eq!(listed.len(), 1);
    assert_eq!(listed[0]["match_id"], usf4["match_id"]);
    assert_eq!(listed[0]["game"], "usf4");
    let (_, plain) = f.bridge.get(f.players[0].token(), "/v1/assignments").await;
    let (_, named) = f
        .bridge
        .get(f.players[0].token(), "/v1/assignments?game=usf4")
        .await;
    assert_eq!(plain["assignments"], named["assignments"]);
    // SFV's list holds 50 of its own and nothing of USF4's.
    let listed = assignments(&f, 0, "?game=sfv").await;
    assert_eq!(listed.len(), 50);
    assert!(listed.iter().all(|row| row["game"] == "sfv" && sfv.contains(&row["match_id"])));
    let (_, found) = f
        .bridge
        .get(&f.organizer, &format!("/v1/matches/{}", sfv[0].as_str().unwrap()))
        .await;
    assert!(found["rules"].get("edition_policy").is_none(), "{found}");
    // An unknown game is refused as the other routes refuse one.
    let (status, body) = f
        .bridge
        .get(f.players[0].token(), "/v1/assignments?game=sf6")
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{body}");
    assert_eq!(body["error"]["message"], "game is not a known game.");
}

#[tokio::test]
async fn a_match_link_says_its_matchs_game() {
    let f = fixture(3).await;
    let link = |id: &Value| {
        let url = f.bridge.url(&format!("/v1/matches/{}/link", id.as_str().unwrap()));
        let client = f.bridge.client.clone();
        async move { common::read(client.get(url).send().await.unwrap()).await }
    };
    let usf4 = f.bracket("usf4-link", 0, 1).await;
    let sfv = f.ordinary_in("sfv", None, "sfv-link", [2, 0], 1).await;
    // No credential, and nothing but whether it is over and its game.
    assert_eq!(
        link(&usf4["match_id"]).await,
        (StatusCode::OK, json!({ "over": false, "game": "usf4" }))
    );
    assert_eq!(
        link(&sfv["match_id"]).await,
        (StatusCode::OK, json!({ "over": false, "game": "sfv" }))
    );
    f.game(sfv["match_id"].as_str().unwrap(), 0).await;
    assert_eq!(
        link(&sfv["match_id"]).await,
        (StatusCode::OK, json!({ "over": true, "game": "sfv" }))
    );
}

/// A first-to-1 USF4 record that player 0 won against player 1, as the
/// bridge answered it before games existed.
fn usf4_record(f: &Fixture, body: &Value) -> Value {
    let recent = &body["recent"][0];
    json!({
        "ember_id": f.id(0),
        "sets": { "played": 1, "won": 1, "lost": 0 },
        "games": { "won": 1, "lost": 0, "drawn": 0 },
        "cancelled": 0,
        "recent": [{
            "match_id": recent["match_id"],
            "result": "won",
            "wins": 1,
            "opponent_wins": 0,
            "games_to_win": 1,
            "opponent": { "ember_id": f.id(1), "participant_id": f.participant(1) },
            "lobby_id": null,
            "tournament_id": null,
            "round_label": null,
            "finished_at": recent["finished_at"],
        }],
        "event_cursor": body["event_cursor"],
    })
}

#[tokio::test]
async fn a_record_is_one_games_and_usf4s_reads_as_it_always_did() {
    let f = fixture(3).await;
    let usf4 = f.bracket("usf4-won", 0, 1).await;
    let (status, body) = f.game(usf4["match_id"].as_str().unwrap(), 0).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");

    // A USF4-only database: the record without `game` is the record from
    // before games, byte for byte, and names no game.
    let before = record_bytes(&f, 0, "").await;
    let parsed: Value = serde_json::from_slice(&before).unwrap();
    assert_eq!(
        before,
        serde_json::to_vec(&usf4_record(&f, &parsed)).unwrap()
    );
    assert_eq!(record_bytes(&f, 0, "?game=usf4").await, before);
    assert_eq!(record_bytes(&f, 0, "?limit=10").await, before);

    // An SFV set player 0 loses counts in SFV's record alone.
    let sfv = f
        .ordinary_in("sfv", None, "sfv-lost", [0, 2], 1)
        .await;
    let (status, body) = f.game(sfv["match_id"].as_str().unwrap(), 1).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    let after = record_bytes(&f, 0, "").await;
    let parsed: Value = serde_json::from_slice(&after).unwrap();
    assert_eq!(
        after,
        serde_json::to_vec(&usf4_record(&f, &parsed)).unwrap()
    );
    assert_eq!(record_bytes(&f, 0, "?game=usf4").await, after);
    let sfv_record: Value =
        serde_json::from_slice(&record_bytes(&f, 0, "?game=sfv").await).unwrap();
    assert_eq!(
        sfv_record["sets"],
        json!({ "played": 1, "won": 0, "lost": 1 })
    );
    assert_eq!(
        sfv_record["games"],
        json!({ "won": 0, "lost": 1, "drawn": 0 })
    );
    assert_eq!(sfv_record["recent"][0]["match_id"], sfv["match_id"]);
    assert_eq!(sfv_record["recent"].as_array().unwrap().len(), 1);

    // The player reads their own, per game, too.
    let path = |query: &str| format!("/v1/players/{}/record{query}", f.id(0));
    let (_, own) = f.bridge.get(f.players[0].token(), &path("?game=sfv")).await;
    assert_eq!(own["sets"], sfv_record["sets"]);

    // An unknown game is refused as the other routes refuse one.
    for query in ["?game=sf6", "?game=", "?game=USF4"] {
        let (status, body) = f.bridge.get(&f.provider, &path(query)).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{query}: {body}");
        assert_eq!(code(&body), "invalid_request");
        assert_eq!(body["error"]["message"], "game is not a known game.");
    }
}

#[tokio::test]
async fn an_sfv_lobbys_sets_are_sfv_matches_and_its_standings_count_them() {
    let f = fixture(2).await;
    let lobby = f.create_in("sfv", "sfv-hill", 1, "winner_stays").await;
    let id = lobby["lobby_id"].as_str().unwrap().to_owned();
    for index in 0..2 {
        let (status, body) = f.join(&id, index).await;
        assert_eq!(status, StatusCode::CREATED, "{body}");
    }
    let set = f.lobby(&id).await["current_match_id"].clone();
    assert_eq!(game_of(&assignments(&f, 0, "?game=sfv").await, &set), "sfv");
    let (_, found) = f
        .bridge
        .get(
            &f.organizer,
            &format!("/v1/matches/{}", set.as_str().unwrap()),
        )
        .await;
    assert!(found["rules"].get("edition_policy").is_none(), "{found}");

    let (status, body) = f.win(&id, 0).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    let standings = f.lobby(&id).await["standings"].clone();
    assert_eq!(standings[0]["sets_won"], 1, "{standings}");
    // The next set is SFV's too.
    let next = f.lobby(&id).await["current_match_id"].clone();
    assert_ne!(next, set);
    assert_eq!(game_of(&assignments(&f, 0, "?game=sfv").await, &next), "sfv");

    // A USF4 lobby's sets stay USF4's.
    let g = fixture(2).await;
    let usf4 = g.create("hill", 1, "winner_stays").await;
    let usf4_id = usf4["lobby_id"].as_str().unwrap().to_owned();
    for index in 0..2 {
        g.join(&usf4_id, index).await;
    }
    let usf4_set = g.lobby(&usf4_id).await["current_match_id"].clone();
    assert_eq!(game_of(&assignments(&g, 0, "").await, &usf4_set), "usf4");
}

#[tokio::test]
async fn an_sfv_tournaments_sets_are_sfv_matches() {
    let f = fixture(2).await;
    let (status, created) = f
        .bridge
        .post_keyed(
            &f.provider,
            "/v1/tournaments",
            json!({
                "external_tournament_id": "sfv-weekly",
                "game": "sfv",
                "format": "single_elimination",
                "games_to_win": 1,
                "required_build_id": "test-build",
                "metadata": {},
            }),
            Some("create-sfv-weekly"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    let id = created["tournament_id"].as_str().unwrap().to_owned();
    for index in 0..2 {
        let (status, body) = f
            .bridge
            .post_keyed(
                &f.provider,
                &format!("/v1/tournaments/{id}/entrants"),
                json!({ "participant_id": f.participant(index), "ember_id": f.id(index) }),
                Some(&format!("register-{index}-{}", rand_key())),
            )
            .await;
        assert_eq!(status, StatusCode::CREATED, "{body}");
    }
    let (_, current) = f
        .bridge
        .get(&f.provider, &format!("/v1/tournaments/{id}"))
        .await;
    let (status, started) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/tournaments/{id}/start"),
            json!({ "expected_revision": current["revision"] }),
            Some("start-sfv-weekly"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{started}");
    let set = started["sets"]
        .as_array()
        .unwrap()
        .iter()
        .find(|set| set["status"] == "playing")
        .unwrap()["match_id"]
        .clone();
    assert_eq!(game_of(&assignments(&f, 1, "?game=sfv").await, &set), "sfv");
}
