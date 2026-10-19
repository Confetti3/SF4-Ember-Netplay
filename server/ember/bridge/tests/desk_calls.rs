//! Discord calls from the desk (`delivery::calls`): an organizer gives their
//! event a Discord channel webhook; the desk posts each set's call there
//! when it creates the set's Ember match and when the organizer calls it
//! again, with the players pinged and the match link, and the score once the
//! set is done. Against a stand-in Discord webhook.
mod common;
#[allow(dead_code, unused_imports)]
#[path = "challonge/support.rs"]
mod support;

use std::{
    collections::VecDeque,
    sync::{Arc, Mutex},
};

use axum::{Router, body::Bytes, extract::Path, routing::post};
use common::{Bridge, Player, sign_in, sites::connect_discord};
use ember_protocol::challenge::{Action, Method};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use support::*;

/// A stand-in Discord webhook: what it was posted, and the statuses to
/// answer with (in order) before it answers 204. A 429 says retry after a
/// second, as Discord does.
#[derive(Clone, Default)]
struct Discord {
    posts: Arc<Mutex<Vec<(String, Json)>>>,
    answers: Arc<Mutex<VecDeque<u16>>>,
}

async fn discord() -> (String, Discord) {
    let fake = Discord::default();
    let shared = fake.clone();
    let app = Router::new().route(
        "/api/webhooks/{id}/{token}",
        post(
            move |Path((id, token)): Path<(String, String)>, body: Bytes| {
                let shared = shared.clone();
                async move {
                    let body = serde_json::from_slice(&body).unwrap();
                    shared
                        .posts
                        .lock()
                        .unwrap()
                        .push((format!("{id}/{token}"), body));
                    let status = shared.answers.lock().unwrap().pop_front().unwrap_or(204);
                    (
                        axum::http::StatusCode::from_u16(status).unwrap(),
                        [("retry-after", "1")],
                    )
                }
            },
        ),
    );
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    (
        format!("http://{address}/api/webhooks/123456789/secret-token"),
        fake,
    )
}

impl Discord {
    fn posts(&self) -> Vec<(String, Json)> {
        self.posts.lock().unwrap().clone()
    }
}

async fn send(bridge: &Bridge, player: &Player, path: &str, command: Json) -> (StatusCode, Json) {
    sign_in::send(
        bridge,
        player,
        Action::SiteConnect,
        Method::Post,
        path,
        command,
    )
    .await
}

#[tokio::test]
async fn the_desk_calls_a_set_on_discord_and_posts_its_score() {
    let fake = fake_challonge();
    fake.lists("/v2.1/tournaments.json", |_| {
        json!({ "data": [{ "id": "9001", "type": "tournament", "attributes": { "name": "Weekly" } }] })
    });
    let bridge = desk_bridge(&fake).await;
    let (hook, discord) = discord().await;
    let player1 = player(&bridge, 1).await;
    let player2 = player(&bridge, 2).await;
    let player3 = player(&bridge, 3).await;
    CHALLONGE.organize(&bridge, &player1, "good-player1").await;
    let (_, started) = CHALLONGE
        .start_event(&bridge, &player1, "9001", json!({}))
        .await;
    let id = started["id"].as_str().unwrap().to_owned();
    let event_path = format!("/v1/organizer/events/{id}");

    // Only a Discord channel webhook is taken, and only from the organizer.
    let (status, _) = send(
        &bridge,
        &player1,
        &event_path,
        json!({ "discord_webhook": "https://evil.example/api/webhooks/1/t" }),
    )
    .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = send(
        &bridge,
        &player2,
        &event_path,
        json!({ "discord_webhook": hook }),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = send(
        &bridge,
        &player1,
        &event_path,
        json!({ "discord_webhook": hook, "post_results": true }),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    // Read back masked: never the token.
    let (_, listed) = bridge.get(player1.token(), "/v1/organizer/events").await;
    let shown = &listed["events"][0];
    assert_eq!(shown["discord_webhook"]["id"], "…6789");
    assert_eq!(shown["post_results"], true);
    assert!(!listed.to_string().contains("secret-token"));
    // Another organizer sees none of it.
    let (_, other) = bridge.get(player2.token(), "/v1/organizer/events").await;
    assert_eq!(other["events"], json!([]));

    // Player 2 has Discord connected in Ember; player 3 has not.
    connect_discord(&bridge, "222", &player2).await;
    let desk = bridge.provider(BRACKETS).await;
    let links = [
        bridge.link(&desk, &player2, BRACKETS, "p2").await,
        bridge.link(&desk, &player3, BRACKETS, "p3").await,
    ];
    let (status, created) = bridge
        .post_keyed(
            &desk,
            "/v1/matches",
            json!({
                "external_match_id": "challonge:9001:s1:1",
                "game": "usf4",
                "participants": [
                    { "participant_id": links[0]["participant_id"], "ember_id": player2.id(), "slot": 0 },
                    { "participant_id": links[1]["participant_id"], "ember_id": player3.id(), "slot": 1 },
                ],
                "rules": {
                    "games_to_win": 3,
                    "draw_policy": "replay_no_score",
                    "native_rules_profile": "ember-room-v1",
                    "edition_policy": "ultra_only",
                    "character_policy": "unrestricted_between_games",
                    "stage_policy": "p1_selects",
                    "input_delay_policy": "ember_existing_ready_policy",
                },
                "observer_policy": "authorized_only",
                "result_policy": "two_player_agreement_or_review",
                "required_build_id": "test-build",
                "metadata": { "round_label": "Losers Quarter-Final" },
            }),
            Some("create-s1"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    let match_id = created["match_id"].as_str().unwrap().to_owned();
    let snapshot = |progress: &str| {
        json!({ "sets": [{ "id": "s1", "round": -3, "phase": "Top 8", "status": "open",
                           "progress": progress, "held": false, "match_id": match_id }] })
    };
    let put = |body: Json| {
        let bridge = &bridge;
        let desk = &desk;
        let id = &id;
        async move {
            bridge
                .client
                .put(bridge.url(&format!("/v1/desk/events/{id}/sets")))
                .bearer_auth(desk)
                .header("content-type", "application/json")
                .body(body.to_string())
                .send()
                .await
                .unwrap()
                .status()
        }
    };

    // The desk's new match is called once, however often the connector reads.
    assert_eq!(put(snapshot("live")).await, StatusCode::OK);
    assert_eq!(put(snapshot("live")).await, StatusCode::OK);
    ember_bridge::deliver_results(bridge.state()).await;
    let posts = discord.posts();
    assert_eq!(posts.len(), 1);
    let (path, call) = &posts[0];
    assert_eq!(path, "123456789/secret-token");
    let content = call["content"].as_str().unwrap();
    assert!(
        content.contains("**Losers Quarter-Final · Top 8 · first to 3**"),
        "{content}"
    );
    assert!(content.contains("(<@222>)"), "{content}");
    assert!(
        content.contains(&format!(
            "/m#{}/{match_id}",
            bridge.state().config.bridge_id
        )),
        "{content}"
    );
    assert_eq!(
        call["allowed_mentions"],
        json!({ "parse": [], "users": ["222"] })
    );
    let sets = || async {
        bridge
            .get(player1.token(), &format!("{event_path}/sets"))
            .await
            .1["sets"][0]
            .clone()
    };
    assert_eq!(sets().await["call"]["posted"], "delivered");

    // Called again: Discord says slow down, and the post goes out after.
    *discord.answers.lock().unwrap() = VecDeque::from([429]);
    let (status, _) = send(
        &bridge,
        &player1,
        &format!("{event_path}/sets/s1"),
        json!({ "call": true }),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    ember_bridge::deliver_results(bridge.state()).await;
    assert_eq!(discord.posts().len(), 2);
    assert_eq!(sets().await["call"]["posted"], "pending");
    bridge.clock.advance(60);
    ember_bridge::deliver_results(bridge.state()).await;
    assert_eq!(discord.posts().len(), 3);
    assert_eq!(sets().await["call"]["posted"], "delivered");

    // Done: the score is posted once, with nobody pinged.
    assert_eq!(put(snapshot("reported")).await, StatusCode::OK);
    assert_eq!(put(snapshot("reported")).await, StatusCode::OK);
    ember_bridge::deliver_results(bridge.state()).await;
    let posts = discord.posts();
    assert_eq!(posts.len(), 4);
    let result = &posts[3].1;
    assert!(
        result["content"]
            .as_str()
            .unwrap()
            .starts_with("**Losers Quarter-Final · Top 8**:"),
        "{result}"
    );
    assert_eq!(result["allowed_mentions"], json!({ "parse": [] }));

    // Taken away, nothing more is posted.
    let (status, _) = send(
        &bridge,
        &player1,
        &event_path,
        json!({ "discord_webhook": null }),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    send(
        &bridge,
        &player1,
        &format!("{event_path}/sets/s1"),
        json!({ "call": true }),
    )
    .await;
    ember_bridge::deliver_results(bridge.state()).await;
    assert_eq!(discord.posts().len(), 4);
}
