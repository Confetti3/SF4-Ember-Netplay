//! `/room` over Discord's HTTP interactions, against a mock bridge and a mock
//! Discord API.
mod support;

use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
    time::Duration,
};

use ed25519_dalek::{Signer, SigningKey};
use ember_notifier::{Config, Discord, Interactions, register_commands};
use reqwest::Method;
use serde_json::{Value, json};
use support::{
    BRIDGE_ID, Logged, Mock, PLAYER, ROOM_ID, base_config, bot, bridge, bridge_slow, created,
    no_room, notifier, refusal, room,
};

const APPLICATION: &str = "app1";
const TOKEN: &str = "interaction-token_1";

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn signing_key() -> SigningKey {
    SigningKey::from_bytes(&[9u8; 32])
}

fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs()
}

/// A notifier serving `/room`, and the URL of its interactions endpoint.
async fn serve(bridge: &Mock, discord: &Mock) -> (String, Vec<tokio::task::JoinHandle<()>>) {
    let mut config = base_config("interactions");
    config.bot = Some(bot(&bridge.origin));
    config.discord = Some(Discord {
        webhook_url: None,
        username: "Ember".into(),
        interactions: Some(Interactions {
            application_id: APPLICATION.into(),
            public_key: hex(signing_key().verifying_key().as_bytes()),
            api_base: discord.origin.clone(),
            bot_token: Some("test-bot-token".into()),
            guild_id: None,
        }),
    });
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let url = format!(
        "http://{}/discord/interactions",
        listener.local_addr().unwrap()
    );
    let (_, tasks) = notifier(config).start(listener).unwrap();
    (url, tasks)
}

/// Posts `body` signed as Discord would at `timestamp`.
async fn post(url: &str, timestamp: u64, body: &str) -> reqwest::Response {
    let timestamp = timestamp.to_string();
    let signature = signing_key().sign(format!("{timestamp}{body}").as_bytes());
    reqwest::Client::new()
        .post(url)
        .header("x-signature-ed25519", hex(&signature.to_bytes()))
        .header("x-signature-timestamp", timestamp)
        .body(body.to_owned())
        .send()
        .await
        .unwrap()
}

fn command(user: Value, options: Value) -> String {
    let mut interaction = json!({
        "type": 2,
        "id": "1",
        "token": TOKEN,
        "data": { "name": "room", "options": options },
    });
    for (key, value) in user.as_object().unwrap() {
        interaction[key] = value.clone();
    }
    interaction.to_string()
}

fn in_guild(id: &str) -> Value {
    json!({ "member": { "user": { "id": id } } })
}

async fn discord() -> Mock {
    Mock::start(|_| (200, "{}".into())).await
}

fn edited(request: &Logged) -> String {
    assert_eq!(
        request.path,
        format!("/webhooks/{APPLICATION}/{TOKEN}/messages/@original")
    );
    assert_eq!(request.body["allowed_mentions"], json!({ "parse": [] }));
    request.body["content"].as_str().unwrap().to_owned()
}

/// A message posted to the channel through the interaction's webhook.
fn followup(request: &Logged) -> String {
    assert_eq!(request.path, format!("/webhooks/{APPLICATION}/{TOKEN}"));
    assert_eq!(request.body["allowed_mentions"], json!({ "parse": [] }));
    request.body["content"].as_str().unwrap().to_owned()
}

/// What Discord is told first, to the person who asked: the deferral is
/// private, since a room's link is posted separately.
fn deferred() -> Value {
    json!({ "type": 5, "data": { "flags": 64 } })
}

/// What reached the channel and what only the person who asked saw.
#[derive(Default)]
struct Channel {
    /// Whether the deferred reply has been filled in yet.
    settled: bool,
    public: Vec<String>,
    private: Vec<String>,
}

/// A Discord stand-in that tracks the deferred reply as Discord does: the
/// first message to arrive fills it in and is private, whether it is an edit
/// or a followup. Edits answer `edit(n)` for the n-th try (from 1); a
/// followup answers `followup`.
async fn tracking_discord(
    edit: impl Fn(usize) -> u16 + Send + Sync + 'static,
    followup: u16,
) -> (Mock, Arc<Mutex<Channel>>) {
    let channel = Arc::new(Mutex::new(Channel::default()));
    let state = channel.clone();
    let edits = AtomicUsize::new(0);
    let mock = Mock::start(move |request| {
        let text = request.body["content"]
            .as_str()
            .unwrap_or_default()
            .to_owned();
        let mut channel = state.lock().unwrap();
        let status = if request.method == Method::PATCH {
            edit(edits.fetch_add(1, Ordering::SeqCst) + 1)
        } else {
            followup
        };
        if status == 200 {
            if request.method == Method::PATCH || !channel.settled {
                channel.private.push(text);
            } else {
                channel.public.push(text);
            }
            channel.settled = true;
        }
        (status, "{}".into())
    })
    .await;
    (mock, channel)
}

async fn open_a_room(bridge: &Mock, discord: &Mock) -> Vec<tokio::task::JoinHandle<()>> {
    let (url, tasks) = serve(bridge, discord).await;
    let body = command(in_guild("42"), json!([]));
    assert_eq!(post(&url, now(), &body).await.status(), 200);
    tasks
}

async fn until(mut done: impl FnMut() -> bool) {
    for _ in 0..200 {
        if done() {
            return;
        }
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    panic!("timed out");
}

#[tokio::test]
async fn a_busy_edit_is_retried_before_the_link_is_posted() {
    for status in [429, 503] {
        let bridge = bridge(created, no_room).await;
        // The first edit fails; the followup must wait for one that works,
        // or it would fill the deferred reply and never reach the channel.
        let (discord, channel) =
            tracking_discord(move |attempt| if attempt == 1 { status } else { 200 }, 200).await;
        let tasks = open_a_room(&bridge, &discord).await;
        until(|| !channel.lock().unwrap().public.is_empty()).await;
        let channel = channel.lock().unwrap();
        assert_eq!(channel.private, ["Your room is open."], "{status}");
        assert_eq!(channel.public.len(), 1, "{status}");
        assert!(channel.public[0].contains(ROOM_ID), "{status}");
        for task in tasks {
            task.abort();
        }
    }
}

#[tokio::test]
async fn an_edit_that_keeps_failing_posts_nothing_to_the_channel() {
    let bridge = bridge(created, no_room).await;
    let (discord, channel) = tracking_discord(|_| 503, 200).await;
    let tasks = open_a_room(&bridge, &discord).await;
    // The edit of the reply, and the try at the link in the private reply.
    until(|| discord.requests().len() == 6).await;
    tokio::time::sleep(Duration::from_millis(300)).await;
    let channel = channel.lock().unwrap();
    assert!(discord.requests().iter().all(|r| r.method == Method::PATCH));
    assert!(channel.public.is_empty() && channel.private.is_empty());
    for task in tasks {
        task.abort();
    }
}

#[tokio::test]
async fn a_followup_that_fails_leaves_the_link_in_the_private_reply() {
    let bridge = bridge(created, no_room).await;
    let (discord, channel) = tracking_discord(|_| 200, 500).await;
    let tasks = open_a_room(&bridge, &discord).await;
    until(|| channel.lock().unwrap().private.len() == 2).await;
    let channel = channel.lock().unwrap();
    assert_eq!(channel.private[0], "Your room is open.");
    assert!(
        channel.private[1].contains(ROOM_ID),
        "{:?}",
        channel.private
    );
    assert!(channel.public.is_empty());
    for task in tasks {
        task.abort();
    }
}

#[tokio::test]
async fn answers_a_ping_and_refuses_what_was_not_signed() {
    let (bridge, discord) = (bridge(created, no_room).await, discord().await);
    let (url, tasks) = serve(&bridge, &discord).await;
    let ping = r#"{"type":1}"#;

    let answer = post(&url, now(), ping).await;
    assert_eq!(answer.status(), 200);
    assert_eq!(answer.json::<Value>().await.unwrap(), json!({ "type": 1 }));

    // The signature covers the timestamp and the body.
    let timestamp = now().to_string();
    let signature = hex(&signing_key()
        .sign(format!("{timestamp}{ping}").as_bytes())
        .to_bytes());
    let send = |timestamp: String, signature: String, body: &str| {
        reqwest::Client::new()
            .post(&url)
            .header("x-signature-ed25519", signature)
            .header("x-signature-timestamp", timestamp)
            .body(body.to_owned())
            .send()
    };
    let tampered = send(timestamp.clone(), signature.clone(), r#"{"type":2}"#)
        .await
        .unwrap();
    assert_eq!(tampered.status(), 401);
    let moved = send((now() + 1).to_string(), signature.clone(), ping)
        .await
        .unwrap();
    assert_eq!(moved.status(), 401);
    let wrong_key = SigningKey::from_bytes(&[3u8; 32])
        .sign(format!("{timestamp}{ping}").as_bytes())
        .to_bytes();
    let forged = send(timestamp, hex(&wrong_key), ping).await.unwrap();
    assert_eq!(forged.status(), 401);
    let unsigned = reqwest::Client::new()
        .post(&url)
        .body(ping)
        .send()
        .await
        .unwrap();
    assert_eq!(unsigned.status(), 401);
    // A correctly signed request from long ago is a replay.
    assert_eq!(post(&url, now() - 3600, ping).await.status(), 401);
    for task in tasks {
        task.abort();
    }
}

#[tokio::test]
async fn a_linked_player_gets_a_room_and_its_link() {
    let (bridge, discord) = (bridge(created, no_room).await, discord().await);
    let (url, tasks) = serve(&bridge, &discord).await;
    let body = command(
        in_guild("42"),
        json!([
            { "name": "name", "type": 3, "value": "Fight Night" },
            { "name": "capacity", "type": 4, "value": 4 },
        ]),
    );
    let answer = post(&url, now(), &body).await;
    assert_eq!(answer.status(), 200);
    assert_eq!(answer.json::<Value>().await.unwrap(), deferred());

    // The private reply is settled first, then the link goes to the channel.
    let announced = followup(&discord.wait_for(Method::POST, TOKEN).await);
    assert!(
        announced.contains(&format!(
            "https://embernetplay.link/r#{BRIDGE_ID}/{ROOM_ID}"
        )),
        "{announced}"
    );
    let sent = discord.requests();
    assert_eq!(edited(&sent[0]), "Your room is open.");
    assert_eq!(sent.len(), 2);
    let requests = bridge.requests();
    let lookup = requests
        .iter()
        .find(|r| r.path == "/v1/players/lookup")
        .unwrap();
    assert_eq!(lookup.headers["authorization"], "Bearer test-credential");
    assert_eq!(lookup.body, json!({ "discord": ["42"] }));
    let create = requests.iter().find(|r| r.path == "/v1/rooms").unwrap();
    assert_eq!(create.headers["authorization"], "Bearer test-credential");
    assert_eq!(
        create.body,
        json!({
            "name": "Fight Night", "capacity": 4, "build_id": "test-build",
            "creator": { "participant_id": "par_1", "ember_id": PLAYER },
        })
    );
    for task in tasks {
        task.abort();
    }
}

#[tokio::test]
async fn a_slow_lookup_still_opens_the_room() {
    // The bridge takes longer to find the player than Discord allows for an
    // answer; the deferral must not wait for it.
    let delay = |request: &Logged| {
        if request.path == "/v1/players/lookup" {
            Duration::from_millis(2500)
        } else {
            Duration::ZERO
        }
    };
    let (bridge, discord) = (bridge_slow(delay, created, no_room).await, discord().await);
    let (url, tasks) = serve(&bridge, &discord).await;
    let body = command(in_guild("42"), json!([]));
    let answer = tokio::time::timeout(Duration::from_secs(1), post(&url, now(), &body))
        .await
        .expect("the deferral waited for the bridge");
    assert_eq!(answer.json::<Value>().await.unwrap(), deferred());
    assert!(bridge.requests().iter().all(|r| r.path != "/v1/rooms"));

    let announced = followup(&discord.wait_for(Method::POST, TOKEN).await);
    assert!(announced.contains(ROOM_ID), "{announced}");
    for task in tasks {
        task.abort();
    }
}

#[tokio::test]
async fn the_default_name_and_capacity_apply_and_a_direct_message_works() {
    let (bridge, discord) = (bridge(created, no_room).await, discord().await);
    let (url, tasks) = serve(&bridge, &discord).await;
    let body = command(json!({ "user": { "id": "42" } }), json!([]));
    assert_eq!(post(&url, now(), &body).await.status(), 200);
    discord.wait_for(Method::PATCH, "/@original").await;
    let create = bridge
        .requests()
        .into_iter()
        .find(|r| r.path == "/v1/rooms")
        .unwrap();
    assert_eq!(create.body["name"], "Fight Night");
    assert_eq!(create.body["capacity"], 8);
    for task in tasks {
        task.abort();
    }
}

#[tokio::test]
async fn someone_not_connected_is_sent_to_connect_discord() {
    let (bridge, discord) = (bridge(created, no_room).await, discord().await);
    let (url, tasks) = serve(&bridge, &discord).await;
    let answer = post(&url, now(), &command(in_guild("77"), json!([]))).await;
    assert_eq!(answer.json::<Value>().await.unwrap(), deferred());
    let reply = edited(&discord.wait_for(Method::PATCH, "/@original").await);
    assert!(
        reply.contains(&format!("https://embernetplay.link/start#{BRIDGE_ID}")),
        "{reply}"
    );
    assert!(
        bridge.requests().iter().all(|r| r.path != "/v1/rooms"),
        "no room is created for someone who is not found"
    );
    for task in tasks {
        task.abort();
    }
}

#[tokio::test]
async fn a_creator_with_a_room_gets_that_room_again() {
    let limit = || {
        (
            409,
            refusal(
                "stale_revision",
                json!({ "reason": "room_limit", "room_id": ROOM_ID }),
            ),
        )
    };
    let existing = || (200, room("open", 3).to_string());
    let (bridge, discord) = (bridge(limit, existing).await, discord().await);
    let (url, tasks) = serve(&bridge, &discord).await;
    assert_eq!(
        post(&url, now(), &command(in_guild("42"), json!([])))
            .await
            .status(),
        200
    );
    let reply = followup(&discord.wait_for(Method::POST, TOKEN).await);
    assert!(reply.contains(ROOM_ID), "{reply}");
    assert!(reply.contains("(3/8)"), "{reply}");
    let get = bridge
        .requests()
        .into_iter()
        .find(|r| r.method == Method::GET && r.path.starts_with("/v1/rooms/"))
        .unwrap();
    assert_eq!(get.path, format!("/v1/rooms/{ROOM_ID}"));
    for task in tasks {
        task.abort();
    }
}

#[tokio::test]
async fn other_refusals_get_a_short_plain_message() {
    let unsupported = || {
        (
            422,
            refusal(
                "incompatible_build",
                json!({ "reason": "unsupported_build" }),
            ),
        )
    };
    let (bridge, discord) = (bridge(unsupported, no_room).await, discord().await);
    let (url, tasks) = serve(&bridge, &discord).await;
    post(&url, now(), &command(in_guild("42"), json!([]))).await;
    let reply = edited(&discord.wait_for(Method::PATCH, "/@original").await);
    assert!(reply.contains("not available for this build"), "{reply}");
    assert!(!reply.contains("http"), "{reply}");

    // A name the rules refuse is answered without asking the bridge.
    let long = "x".repeat(65);
    let body = command(
        in_guild("42"),
        json!([{ "name": "name", "type": 3, "value": long }]),
    );
    let creates = bridge
        .requests()
        .iter()
        .filter(|r| r.path == "/v1/rooms")
        .count();
    post(&url, now(), &body).await;
    let replies = loop {
        let replies = discord.requests();
        if replies.len() == 2 {
            break replies;
        }
        tokio::time::sleep(std::time::Duration::from_millis(25)).await;
    };
    assert!(edited(&replies[1]).contains("1 to 64 characters"));
    assert_eq!(
        bridge
            .requests()
            .iter()
            .filter(|r| r.path == "/v1/rooms")
            .count(),
        creates
    );
    for task in tasks {
        task.abort();
    }
}

/// Each scope's registered commands, by the scope's path.
type Scopes = Arc<Mutex<Vec<(String, Vec<Value>)>>>;

/// A stand-in for Discord's command registry: each scope's list, which a PUT
/// replaces and a POST adds to or updates by name and type.
fn registry(scopes: Scopes) -> impl Fn(&Logged) -> (u16, String) {
    move |request| {
        let mut scopes = scopes.lock().unwrap();
        let index = match scopes.iter().position(|(path, _)| *path == request.path) {
            Some(index) => index,
            None => {
                scopes.push((request.path.clone(), Vec::new()));
                scopes.len() - 1
            }
        };
        let list = &mut scopes[index].1;
        if request.method == Method::PUT {
            *list = request.body.as_array().cloned().unwrap_or_default();
        } else if request.method == Method::POST {
            let same =
                |c: &Value| c["name"] == request.body["name"] && c["type"] == request.body["type"];
            list.retain(|c| !same(c));
            list.push(request.body.clone());
        }
        (200, request.body.to_string())
    }
}

#[tokio::test]
async fn the_command_is_registered_for_the_application_or_a_server() {
    let others = || {
        vec![
            json!({ "name": "rank", "type": 1, "description": "Another bot's command" }),
            json!({ "name": "Report", "type": 3 }),
        ]
    };
    let scopes = Arc::new(Mutex::new(vec![
        ("/applications/app1/commands".to_owned(), others()),
        ("/applications/app1/guilds/g9/commands".to_owned(), others()),
    ]));
    let discord = Mock::start(registry(scopes.clone())).await;
    let mut config: Config = base_config("register");
    config.discord = Some(Discord {
        webhook_url: None,
        username: "Ember".into(),
        interactions: Some(Interactions {
            application_id: APPLICATION.into(),
            public_key: hex(signing_key().verifying_key().as_bytes()),
            api_base: discord.origin.clone(),
            bot_token: Some("test-bot-token".into()),
            guild_id: None,
        }),
    });
    let names = |path: &str| -> Vec<String> {
        let scopes = scopes.lock().unwrap();
        let (_, list) = scopes.iter().find(|(p, _)| p == path).unwrap();
        let mut names: Vec<String> = list
            .iter()
            .map(|c| c["name"].as_str().unwrap().to_owned())
            .collect();
        names.sort();
        names
    };
    let expected = vec!["Report".to_owned(), "rank".to_owned(), "room".to_owned()];
    // Registered twice in each scope: the second updates /room, and the
    // application's other commands stay.
    for _ in 0..2 {
        register_commands(&config).await.unwrap();
    }
    let request = discord
        .wait_for(Method::POST, "/applications/app1/commands")
        .await;
    assert_eq!(request.headers["authorization"], "Bot test-bot-token");
    assert_eq!(request.body["name"], "room");
    assert_eq!(request.body["options"][0]["name"], "name");
    assert_eq!(request.body["options"][1]["max_value"], 16);
    assert_eq!(names("/applications/app1/commands"), expected);

    config
        .discord
        .as_mut()
        .and_then(|d| d.interactions.as_mut())
        .unwrap()
        .guild_id = Some("g9".into());
    for _ in 0..2 {
        register_commands(&config).await.unwrap();
    }
    assert_eq!(names("/applications/app1/guilds/g9/commands"), expected);
    assert!(
        discord.requests().iter().all(|r| r.method == Method::POST),
        "registration replaced a command list"
    );

    let refused = Mock::start(|_| (401, "{}".into())).await;
    config
        .discord
        .as_mut()
        .and_then(|d| d.interactions.as_mut())
        .unwrap()
        .api_base = refused.origin.clone();
    assert!(register_commands(&config).await.is_err());
}
