//! `/tournament` and its buttons over Discord's HTTP interactions, against a
//! mock bracket site and a mock Discord API.
mod support;

use ed25519_dalek::{Signer, SigningKey};
use ember_notifier::{Brackets, Discord, Interactions};
use reqwest::Method;
use serde_json::{Value, json};
use support::{Mock, base_config, bot, notifier};

const APPLICATION: &str = "app1";
const TOKEN: &str = "interaction-token_1";
const PLAYER: &str = "274220342558756145";
const ORGANIZER: &str = "80351110224678912";

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn signing_key() -> SigningKey {
    SigningKey::from_bytes(&[9u8; 32])
}

/// A notifier serving `/tournament` for the site at `site`; its interactions URL.
async fn serve(site: &Mock, discord: &Mock) -> String {
    let mut config = base_config("tournament");
    config.bot = Some(bot("http://127.0.0.1:1"));
    config.brackets = Some(Brackets {
        base: site.origin.clone(),
        token: "site-bot-token".into(),
        feed: false,
        feed_secs: 60,
    });
    config.discord = Some(Discord {
        webhook_url: None,
        username: "Ember".into(),
        interactions: Some(Interactions {
            application_id: APPLICATION.into(),
            public_key: hex(signing_key().verifying_key().as_bytes()),
            api_base: discord.origin.clone(),
            bot_token: None,
            guild_id: None,
        }),
    });
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let url = format!(
        "http://{}/discord/interactions",
        listener.local_addr().unwrap()
    );
    notifier(config).start(listener).unwrap();
    url
}

/// Sends `interaction` signed as Discord would; its answer.
async fn send(url: &str, interaction: Value) -> Value {
    let body = interaction.to_string();
    let timestamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs()
        .to_string();
    let signature = signing_key().sign(format!("{timestamp}{body}").as_bytes());
    let response = reqwest::Client::new()
        .post(url)
        .header("x-signature-ed25519", hex(&signature.to_bytes()))
        .header("x-signature-timestamp", timestamp)
        .body(body)
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), 200);
    response.json().await.unwrap()
}

fn from_player(mut interaction: Value) -> Value {
    interaction["token"] = json!(TOKEN);
    interaction["member"] =
        json!({ "user": { "id": PLAYER, "username": "kate", "global_name": "Kate" } });
    interaction
}

/// The deferred reply's edit, once it comes.
async fn edited(discord: &Mock) -> Value {
    let edit = discord
        .wait_for(
            Method::PATCH,
            &format!("/webhooks/{APPLICATION}/{TOKEN}/messages/@original"),
        )
        .await;
    assert_eq!(edit.body["allowed_mentions"], json!({ "parse": [] }));
    edit.body
}

#[tokio::test]
async fn a_player_signs_up_from_the_command_as_themselves() {
    let site = Mock::start(|_| {
        (
            201,
            json!({ "discord": PLAYER, "name": "Kate", "ember_id": null, "checked_in": false })
                .to_string(),
        )
    })
    .await;
    let discord = Mock::start(|_| (200, "{}".into())).await;
    let url = serve(&site, &discord).await;
    let command = from_player(json!({
        "type": 2,
        "data": { "name": "tournament", "options": [{ "type": 1, "name": "signup", "options": [{ "name": "id", "value": "weekly" }] }] },
    }));
    assert_eq!(
        send(&url, command).await,
        json!({ "type": 5, "data": { "flags": 64 } })
    );
    let content = edited(&discord).await["content"]
        .as_str()
        .unwrap()
        .to_owned();
    assert!(content.starts_with("You are signed up"), "{content}");
    assert!(
        content.contains("Connect Discord in Ember"),
        "not linked yet: {content}"
    );

    let asked = &site.requests()[0];
    assert_eq!(
        (asked.method.clone(), asked.path.as_str()),
        (Method::POST, "/tournaments/weekly/entrants")
    );
    assert_eq!(asked.headers["authorization"], "Bearer site-bot-token");
    assert_eq!(asked.headers["x-discord-user"], PLAYER);
    assert_eq!(asked.body, json!({ "name": "Kate" }));
}

#[tokio::test]
async fn a_shown_tournament_has_buttons_and_a_press_checks_in() {
    let site = Mock::start(|request| match request.method {
        Method::GET => (
            200,
            json!({ "title": "Weekly", "state": "check_in", "games_to_win": 2, "entrants": [{}], "organizers": [ORGANIZER] })
                .to_string(),
        ),
        _ if request.path.ends_with("/check-in") => (
            409,
            json!({ "error": "check-in is not open while live" }).to_string(),
        ),
        _ => (500, "{}".into()),
    })
    .await;
    let discord = Mock::start(|_| (200, "{}".into())).await;
    let url = serve(&site, &discord).await;
    let mut show = from_player(json!({
        "type": 2,
        "data": { "name": "tournament", "options": [{ "type": 1, "name": "show", "options": [{ "name": "id", "value": "weekly" }] }] },
    }));
    // A member who manages the server shows it to the whole channel.
    show["member"]["permissions"] = json!("32");
    assert_eq!(
        send(&url, show).await,
        json!({ "type": 5, "data": { "flags": 0 } })
    );
    let message = edited(&discord).await;
    assert!(
        message["content"]
            .as_str()
            .unwrap()
            .starts_with("**Weekly**: check-in is open.")
    );
    let button = &message["components"][0]["components"][0];
    assert_eq!(button["custom_id"], "tournament:checkin:weekly");

    // Pressed, by someone else as it happens: they act as themselves, and
    // the site's refusal is what they see.
    let press = from_player(
        json!({ "type": 3, "data": { "custom_id": "tournament:checkin:weekly", "component_type": 2 } }),
    );
    assert_eq!(
        send(&url, press).await,
        json!({ "type": 5, "data": { "flags": 64 } })
    );
    let check_in = site
        .wait_for(Method::POST, "/tournaments/weekly/check-in")
        .await;
    assert_eq!(check_in.headers["x-discord-user"], PLAYER);
    for _ in 0..200 {
        let edits: Vec<_> = discord
            .requests()
            .into_iter()
            .filter(|request| request.method == Method::PATCH)
            .collect();
        if edits.len() == 2 {
            assert_eq!(
                edits[1].body["content"],
                "That did not work: check-in is not open while live."
            );
            return;
        }
        tokio::time::sleep(std::time::Duration::from_millis(25)).await;
    }
    panic!("the press was not answered");
}

#[tokio::test]
async fn only_managers_and_its_organizers_post_a_tournament() {
    let site = Mock::start(|_| {
        (
            200,
            json!({ "title": "Weekly", "state": "sign_ups", "games_to_win": 2, "entrants": [], "organizers": [ORGANIZER] })
                .to_string(),
        )
    })
    .await;
    let discord = Mock::start(|_| (200, "{}".into())).await;
    let url = serve(&site, &discord).await;
    let show = |user: &str| {
        let mut show = from_player(json!({
            "type": 2,
            "data": { "name": "tournament", "options": [{ "type": 1, "name": "show", "options": [{ "name": "id", "value": "weekly" }] }] },
        }));
        show["member"]["user"]["id"] = json!(user);
        show["member"]["permissions"] = json!("0");
        show
    };
    // Anyone else is told privately, and nothing reaches the channel.
    assert_eq!(
        send(&url, show(PLAYER)).await,
        json!({ "type": 5, "data": { "flags": 64 } })
    );
    assert_eq!(
        edited(&discord).await["content"],
        "Only the server's managers and the tournament's organizers can post it."
    );
    // Its organizer: a private note, then the tournament as a public followup.
    assert_eq!(
        send(&url, show(ORGANIZER)).await,
        json!({ "type": 5, "data": { "flags": 64 } })
    );
    let posted = discord
        .wait_for(Method::POST, &format!("/webhooks/{APPLICATION}/{TOKEN}"))
        .await;
    assert!(
        posted.body["content"]
            .as_str()
            .unwrap()
            .starts_with("**Weekly**: sign-ups are open.")
    );
    assert_eq!(
        posted.body["components"][0]["components"][0]["label"],
        "Sign up"
    );
    let notes: Vec<_> = discord
        .requests()
        .into_iter()
        .filter(|request| request.method == Method::PATCH)
        .map(|request| request.body["content"].clone())
        .collect();
    assert_eq!(notes.last().unwrap(), "Posted.");
}

#[tokio::test]
async fn a_live_tournaments_news_is_posted_once_as_one_message() {
    let site = Mock::start(|request| match request.path.as_str() {
        "/live" => (200, json!({ "tournaments": [{ "id": "weekly", "last": 2 }, { "id": "quiet", "last": 0 }] }).to_string()),
        _ if request.path.starts_with("/tournaments/weekly/feed") => (
            200,
            json!({ "items": [{ "n": 1, "text": "Semifinals: P3 beats P2 2-1." }, { "n": 2, "text": "Upset: seed 3 *P3* knocks out seed 2." }] }).to_string(),
        ),
        _ => (404, "{}".into()),
    })
    .await;
    let discord = Mock::start(|_| (200, "{}".into())).await;
    let mut config = base_config("feed");
    config.discord = Some(Discord {
        webhook_url: Some(format!("{}/hook", discord.origin)),
        username: "Ember".into(),
        interactions: None,
    });
    config.brackets = Some(Brackets {
        base: site.origin.clone(),
        token: "site-bot-token".into(),
        feed: true,
        feed_secs: 3600,
    });
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let (_, tasks) = notifier(config.clone()).start(listener).unwrap();
    let posted = discord.wait_for(Method::POST, "/hook").await;
    assert_eq!(
        posted.body["content"],
        "Semifinals: P3 beats P2 2-1.\nUpset: seed 3 \\*P3\\* knocks out seed 2."
    );
    assert_eq!(posted.body["allowed_mentions"], json!({ "parse": [] }));
    // Its answer is read, and what was posted kept, a moment after it arrives.
    tokio::time::sleep(std::time::Duration::from_millis(300)).await;
    assert!(
        site.requests()
            .iter()
            .all(|request| !request.path.contains("quiet")),
        "nothing new there"
    );
    for task in tasks {
        task.abort();
    }
    // Started again on the same database: nothing is posted twice.
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let (_, tasks) = notifier(config).start(listener).unwrap();
    tokio::time::sleep(std::time::Duration::from_millis(500)).await;
    assert_eq!(discord.requests().len(), 1);
    for task in tasks {
        task.abort();
    }
}

#[tokio::test]
async fn a_vote_is_posted_as_menus_and_a_pick_is_the_pickers_vote() {
    let site =
        Mock::start(|request| {
            match (request.method.clone(), request.path.as_str()) {
        (Method::GET, "/tournaments/weekly/vote") => (
            200,
            json!({ "closed_at": null, "closes_at": "2026-10-23T20:00:00.000Z", "nominations": [
                { "id": "n1", "category": "best_player", "title": "Kate", "accepted": true },
                { "id": "n2", "category": "best_combo", "title": "Waiting", "accepted": false },
            ] })
            .to_string(),
        ),
        (Method::GET, "/tournaments/weekly") => (200, json!({ "organizers": [] }).to_string()),
        (Method::POST, "/tournaments/weekly/votes/best_player") => (204, String::new()),
        _ => (404, "{}".into()),
    }
        })
        .await;
    let discord = Mock::start(|_| (200, "{}".into())).await;
    let url = serve(&site, &discord).await;
    let mut vote = from_player(json!({
        "type": 2,
        "data": { "name": "tournament", "options": [{ "type": 1, "name": "vote", "options": [{ "name": "id", "value": "weekly" }] }] },
    }));
    vote["member"]["permissions"] = json!("32");
    assert_eq!(
        send(&url, vote).await,
        json!({ "type": 5, "data": { "flags": 0 } })
    );
    let message = edited(&discord).await;
    // One menu: only accepted nominations, and only categories that have one.
    let rows = message["components"].as_array().unwrap();
    assert_eq!(rows.len(), 1);
    let menu = &rows[0]["components"][0];
    assert_eq!(menu["custom_id"], "tournament:pick:weekly:best_player");
    assert_eq!(menu["options"], json!([{ "label": "Kate", "value": "n1" }]));

    let pick = from_player(
        json!({ "type": 3, "data": { "custom_id": "tournament:pick:weekly:best_player", "component_type": 3, "values": ["n1"] } }),
    );
    assert_eq!(
        send(&url, pick).await,
        json!({ "type": 5, "data": { "flags": 64 } })
    );
    let cast = site
        .wait_for(Method::POST, "/tournaments/weekly/votes/best_player")
        .await;
    assert_eq!(
        (cast.headers["x-discord-user"].to_str().unwrap(), &cast.body),
        (PLAYER, &json!({ "nomination": "n1" }))
    );
}
