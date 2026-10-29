//! Discord calls from the desk (`delivery::calls`): an organizer gives their
//! event a Discord channel webhook; the desk posts each set's call there
//! when it creates the set's Ember match and when the organizer calls it
//! again, with the players pinged and the match link, and the score once the
//! set is done. Against a stand-in Discord webhook.
mod common;

use std::{
    collections::VecDeque,
    sync::{Arc, Mutex},
};

use axum::{Router, body::Bytes, extract::Path, routing::post};
use common::challonge::*;
use common::{Bridge, Player, sign_in, sites::connect_discord};
use ember_protocol::challenge::{Action, Method};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};

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

    // Player 2 has Discord connected in Ember; player 3 has not, only on
    // the bracket site, which the connector's snapshot carries.
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
                           "progress": progress, "held": false, "match_id": match_id,
                           "discord": ["999", "333"] }] })
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
    assert!(content.contains("(<@333>)"), "{content}");
    assert!(!content.contains("999"), "{content}");
    assert!(
        content.contains(&format!(
            "/m#{}/{match_id}",
            bridge.state().config.bridge_id
        )),
        "{content}"
    );
    assert_eq!(
        call["allowed_mentions"],
        json!({ "parse": [], "users": ["222", "333"] })
    );
    let sets = || async {
        bridge
            .get(player1.token(), &format!("{event_path}/sets"))
            .await
            .1["sets"][0]
            .clone()
    };
    assert_eq!(sets().await["call"]["posted"], "delivered");

    // Called again at once: refused, so a desk cannot flood the channel.
    let (status, _) = send(
        &bridge,
        &player1,
        &format!("{event_path}/sets/s1"),
        json!({ "call": true }),
    )
    .await;
    assert_eq!(status, StatusCode::TOO_MANY_REQUESTS);
    bridge.clock.advance(120);

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

    // A drawn game, decided at the desk, scores for nobody: the set's score
    // stays, and the draw is counted beside it.
    let adjudicate = |command: Json| {
        let path = format!("{event_path}/matches/{match_id}/adjudications");
        let (bridge, player1) = (&bridge, &player1);
        async move { send(bridge, player1, &path, command).await }
    };
    let revision = sets().await["revision"].clone();
    let (status, decided) = adjudicate(json!({ "kind": "game_result", "draw": true,
        "reason": "both died", "expected_revision": revision }))
    .await;
    assert_eq!(status, StatusCode::CREATED, "{decided}");
    let revision = sets().await["revision"].clone();
    let (status, _) = adjudicate(json!({ "kind": "game_result", "winner_slot": 0,
        "reason": "won", "expected_revision": revision }))
    .await;
    assert_eq!(status, StatusCode::CREATED);
    let set = sets().await;
    assert_eq!(set["score"], json!([1, 0]));
    assert_eq!(set["draws"], 1);

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
    assert!(
        result["content"].as_str().unwrap().ends_with("(1 draw)"),
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
    bridge.clock.advance(120);
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

/// A stand-in for Discord's API as the bridge's bot uses it: it opens a DM
/// channel for a user and takes a message there, recording each message with
/// its user and the bot's authorization, and answering a message with the
/// statuses queued (in order) before 200.
#[derive(Clone, Default)]
struct Bot {
    messages: Arc<Mutex<Vec<(String, String, Json)>>>,
    answers: Arc<Mutex<VecDeque<u16>>>,
}

/// The bot's messages once there are at least `count`, for at most ten
/// seconds: the bridge's own delivery worker may be the one sending them.
async fn bot_messages(bot: &Bot, count: usize) -> Vec<(String, String, Json)> {
    for _ in 0..200 {
        let messages = bot.messages.lock().unwrap().clone();
        if messages.len() >= count {
            return messages;
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    panic!("expected {count} messages from the bot");
}

async fn bot() -> (String, Bot) {
    use axum::http::HeaderMap;
    let fake = Bot::default();
    let shared = fake.clone();
    let app = Router::new()
        .route(
            "/api/v10/users/@me/channels",
            post(|body: Bytes| async move {
                let body: Json = serde_json::from_slice(&body).unwrap();
                let user = body["recipient_id"].as_str().unwrap().to_owned();
                (
                    [("content-type", "application/json")],
                    json!({ "id": format!("9{user}"), "type": 1 }).to_string(),
                )
            }),
        )
        .route(
            "/api/v10/channels/{channel}/messages",
            post(
                move |Path(channel): Path<String>, headers: HeaderMap, body: Bytes| {
                    let shared = shared.clone();
                    async move {
                        let auth = headers["authorization"].to_str().unwrap().to_owned();
                        let user = channel.trim_start_matches('9').to_owned();
                        let body = serde_json::from_slice(&body).unwrap();
                        shared.messages.lock().unwrap().push((user, auth, body));
                        let status = shared.answers.lock().unwrap().pop_front().unwrap_or(200);
                        (
                            axum::http::StatusCode::from_u16(status).unwrap(),
                            [("content-type", "application/json")],
                            json!({ "id": "1" }).to_string(),
                        )
                    }
                },
            ),
        );
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    (format!("http://{address}/api"), fake)
}

// With the Discord app's bot, a called set's players each get its match link
// privately, webhook or not. A player without Discord connected, or whom
// Discord will not let the bot reach, is named on the desk.
#[tokio::test]
async fn a_called_set_messages_each_player_privately() {
    let fake = fake_challonge();
    fake.lists("/v2.1/tournaments.json", |_| {
        json!({ "data": [{ "id": "9001", "type": "tournament", "attributes": { "name": "Weekly" } }] })
    });
    let (api, bot) = bot().await;
    let mut site = challonge_site(&fake.serve().await);
    site.limits = Some(common::sites::LIMITS);
    let mut secrets = with_secret();
    secrets.discord_bot_token = Some(zeroize::Zeroizing::new("bot-token".into()));
    secrets.discord_client_secret = Some(zeroize::Zeroizing::new("client-secret".into()));
    let bridge = Bridge::start_with(
        |config| {
            config.desk = Some(ember_bridge::config::Desk {
                connection: BRACKETS.into(),
                invite_only: false,
            });
            config.discord = Some(ember_bridge::config::Discord {
                client_id: "123456789012345678".into(),
                api_base: api,
            });
            config.sign_in_sites = vec![site];
            config.integration_secrets = Some("unused-in-tests.json".into());
            config.tenants.push(ember_bridge::config::Tenant {
                id: "cups".into(),
                name: "Cups".into(),
                connections: vec![common::sites::direct(BRACKETS, false, &[SITE])],
            });
        },
        secrets,
    )
    .await;
    bridge.clock.freeze();
    let player1 = player(&bridge, 1).await;
    let player2 = player(&bridge, 2).await;
    let player3 = player(&bridge, 3).await;
    CHALLONGE.organize(&bridge, &player1, "good-player1").await;
    let (_, started) = CHALLONGE
        .start_event(&bridge, &player1, "9001", json!({}))
        .await;
    let id = started["id"].as_str().unwrap().to_owned();
    let event_path = format!("/v1/organizer/events/{id}");
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
                    "games_to_win": 2,
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
                "metadata": { "round_label": "Winners Final" },
            }),
            Some("create-s1"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    let match_id = created["match_id"].as_str().unwrap().to_owned();
    let snapshot = json!({ "sets": [{ "id": "s1", "round": 3, "status": "open",
        "progress": "live", "held": false, "match_id": match_id,
        "names": ["Ashgrove", "Minnow"] }] });
    let sent = bridge
        .client
        .put(bridge.url(&format!("/v1/desk/events/{id}/sets")))
        .bearer_auth(&desk)
        .header("content-type", "application/json")
        .body(snapshot.to_string())
        .send()
        .await
        .unwrap();
    assert_eq!(sent.status(), StatusCode::OK);
    ember_bridge::deliver_results(bridge.state()).await;

    // Player 2 gets the link from the bot; player 3 has no Discord.
    let messages = bot_messages(&bot, 1).await;
    assert_eq!(messages.len(), 1, "{messages:?}");
    let (user, auth, message) = &messages[0];
    assert_eq!(user, "222");
    assert_eq!(auth, "Bot bot-token");
    let content = message["content"].as_str().unwrap();
    assert!(
        content.starts_with("Your set is ready: **Winners Final · first to 2**"),
        "{content}"
    );
    assert!(
        content.contains(&format!(
            "/m#{}/{match_id}",
            bridge.state().config.bridge_id
        )),
        "{content}"
    );
    assert_eq!(message["allowed_mentions"], json!({ "parse": [] }));
    let set = || async {
        bridge
            .get(player1.token(), &format!("{event_path}/sets"))
            .await
            .1["sets"][0]
            .clone()
    };
    // The bridge's own delivery worker may hold a message a moment longer.
    let dms = |expected: Json| async move {
        for _ in 0..200 {
            if set().await["dms"] == expected {
                return;
            }
            tokio::time::sleep(std::time::Duration::from_millis(50)).await;
        }
        panic!("the desk never showed {expected}: {}", set().await);
    };
    dms(json!(["delivered", "no_discord"])).await;

    // Called again while player 2 takes no messages from the bot: not tried
    // again, and the desk says so.
    *bot.answers.lock().unwrap() = VecDeque::from([403]);
    bridge.clock.advance(120);
    let (status, _) = send(
        &bridge,
        &player1,
        &format!("{event_path}/sets/s1"),
        json!({ "call": true }),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    ember_bridge::deliver_results(bridge.state()).await;
    dms(json!(["failed", "no_discord"])).await;
    assert_eq!(bot.messages.lock().unwrap().len(), 2);
    bridge.clock.advance(600);
    ember_bridge::deliver_results(bridge.state()).await;
    assert_eq!(bot.messages.lock().unwrap().len(), 2);
}
