//! Discord sign-in against a stand-in Discord: starting needs a proof, the
//! callback takes each sign-in once and in time, the latest sign-in wins, an
//! account moves from another Ember ID only once the player confirms it, a
//! disconnect, cancel or newer sign-in ends sign-ins in flight, a platform's
//! links follow the account, and a bridge without Discord offers none of it.
mod common;

use axum::{
    Form, Router,
    http::{HeaderMap, StatusCode as AxumStatus, header},
    routing::{get, post},
};
use common::{Bridge, Player};
use ember_bridge::{config, integrations::Secrets};
use ember_protocol::challenge::{Action, Method};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use tokio::sync::Notify;
use zeroize::Zeroizing;

const KATE: &str = "274220342558756145";
const SAM: &str = "1725265127372152396";
/// A platform that finds players by Discord account.
const BLUMINT: &str = "bm-partner";

/// A code the stand-in Discord holds until the test lets it go: `arrived`
/// says Discord has it, `release` lets it sign Kate in. One per test, so
/// tests holding a code run side by side.
struct Gate {
    arrived: Notify,
    release: Notify,
}

impl Gate {
    const fn new() -> Self {
        Self {
            arrived: Notify::const_new(),
            release: Notify::const_new(),
        }
    }
}

static SLOW: Gate = Gate::new();
static HELD: Gate = Gate::new();
static LATE: Gate = Gate::new();

/// A stand-in for Discord's token and user endpoints. Codes `good-kate`,
/// `good-sam` and the gated `slow-kate`, `held-kate` and `late-kate` sign in
/// those users; anything else is refused.
async fn fake_discord() -> String {
    type Answer = (AxumStatus, [(header::HeaderName, &'static str); 1], String);
    fn answer(status: AxumStatus, body: Json) -> Answer {
        (
            status,
            [(header::CONTENT_TYPE, "application/json")],
            body.to_string(),
        )
    }
    async fn token(Form(form): Form<std::collections::HashMap<String, String>>) -> Answer {
        let valid = form.get("grant_type").map(String::as_str) == Some("authorization_code")
            && form.get("client_secret").map(String::as_str) == Some("test-secret")
            && form.get("client_id").map(String::as_str) == Some("1546980049692135514")
            && form
                .get("redirect_uri")
                .is_some_and(|uri| uri.ends_with("/v1/discord/callback"));
        let code = form.get("code").map(String::as_str);
        let gate = match code {
            Some("slow-kate") => Some(&SLOW),
            Some("held-kate") => Some(&HELD),
            Some("late-kate") => Some(&LATE),
            _ => None,
        };
        if let Some(gate) = gate {
            gate.arrived.notify_one();
            gate.release.notified().await;
        }
        let user = match code {
            Some("good-kate") => "kate",
            Some(_) if gate.is_some() => "kate",
            Some("good-sam") => "sam",
            _ => "",
        };
        if valid && !user.is_empty() {
            answer(
                AxumStatus::OK,
                json!({ "access_token": format!("token-{user}"), "token_type": "Bearer", "scope": "identify" }),
            )
        } else {
            answer(AxumStatus::BAD_REQUEST, json!({ "error": "invalid_grant" }))
        }
    }
    async fn me(headers: HeaderMap) -> Answer {
        match headers
            .get("authorization")
            .and_then(|value| value.to_str().ok())
        {
            Some("Bearer token-kate") => {
                answer(AxumStatus::OK, json!({ "id": KATE, "username": "kate" }))
            }
            Some("Bearer token-sam") => {
                answer(AxumStatus::OK, json!({ "id": SAM, "username": "sam" }))
            }
            _ => answer(AxumStatus::UNAUTHORIZED, json!({})),
        }
    }
    let app = Router::new()
        .route("/api/oauth2/token", post(token))
        .route("/api/users/@me", get(me));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let base = format!("http://{}/api", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await });
    base
}

async fn bridge_with_discord() -> Bridge {
    bridge_with_discord_secret(Secrets {
        discord_client_secret: Some(Zeroizing::new("test-secret".into())),
        ..Default::default()
    })
    .await
}

/// A bridge configured for Discord and BluMint, with these secrets.
async fn bridge_with_discord_secret(secrets: Secrets) -> Bridge {
    let api_base = fake_discord().await;
    Bridge::start_with(
        |config| {
            config.discord = Some(config::Discord {
                client_id: "1546980049692135514".into(),
                api_base,
            });
            config.integration_secrets = Some("unused-in-tests.json".into());
            config.tenants.push(config::Tenant {
                id: "bm".into(),
                name: "BluMint".into(),
                connections: vec![config::Connection {
                    id: BLUMINT.into(),
                    kind: "blumint".into(),
                    environment: "staging".into(),
                    display_name: "BluMint (test)".into(),
                    enabled: true,
                    api_base: None,
                }],
            });
        },
        secrets,
    )
    .await
}

async fn player(bridge: &Bridge, byte: u8) -> Player {
    let mut player = bridge.player(byte);
    bridge.open_session(&mut player).await;
    player
}

/// Starts a sign-in and returns the state Discord would hand back.
async fn start(bridge: &Bridge, player: &Player) -> String {
    let body = bridge
        .prove(
            player,
            Action::DiscordConnect,
            Method::Post,
            "/v1/discord/start",
            json!({}),
        )
        .await;
    let (status, started) = bridge
        .send_proof(player, Method::Post, "/v1/discord/start", body)
        .await;
    assert_eq!(status, StatusCode::CREATED, "{started}");
    let url = url::Url::parse(started["authorize_url"].as_str().unwrap()).unwrap();
    assert_eq!(url.path(), "/oauth2/authorize");
    let query: std::collections::HashMap<_, _> = url.query_pairs().into_owned().collect();
    assert_eq!(query["scope"], "identify");
    assert_eq!(query["client_id"], "1546980049692135514");
    assert_eq!(query["redirect_uri"], bridge.url("/v1/discord/callback"));
    query["state"].clone()
}

/// What the browser sees when Discord sends it back.
async fn back(bridge: &Bridge, query: &str) -> String {
    let response = bridge
        .client
        .get(bridge.url(&format!("/v1/discord/callback?{query}")))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    response.text().await.unwrap()
}

async fn account(bridge: &Bridge, player: &Player) -> Json {
    let (status, body) = bridge.get(player.token(), "/v1/discord").await;
    assert_eq!(status, StatusCode::OK, "{body}");
    body["account"].clone()
}

/// Signs `player` in as `who` (`kate` or `sam`).
async fn connect(bridge: &Bridge, player: &Player, who: &str) {
    let state = start(bridge, player).await;
    let page = back(bridge, &format!("code=good-{who}&state={state}")).await;
    assert!(page.contains("Discord connected"), "{page}");
}

/// The player's answer on the page that asks before moving an account.
async fn decide(bridge: &Bridge, state: &str, choice: &str) -> String {
    let response = bridge
        .client
        .post(bridge.url("/v1/discord/callback"))
        .header("content-type", "application/x-www-form-urlencoded")
        .body(format!("state={state}&choice={choice}"))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    response.text().await.unwrap()
}

/// Signs `player` in as `who`, whose account another Ember ID has, and moves it.
async fn move_here(bridge: &Bridge, player: &Player, who: &str) {
    let state = start(bridge, player).await;
    let page = back(bridge, &format!("code=good-{who}&state={state}")).await;
    assert!(page.contains("Move this Discord account?"), "{page}");
    let page = decide(bridge, &state, "move").await;
    assert!(page.contains("Discord connected"), "{page}");
}

/// Ends the player's sign-ins in flight, as Ember's Cancel does.
async fn cancel(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    let body = bridge
        .prove(
            player,
            Action::DiscordCancel,
            Method::Delete,
            "/v1/discord/start",
            json!({}),
        )
        .await;
    bridge
        .send_proof(player, Method::Delete, "/v1/discord/start", body)
        .await
}

async fn disconnect(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    let body = bridge
        .prove(
            player,
            Action::DiscordRemove,
            Method::Delete,
            "/v1/discord",
            json!({}),
        )
        .await;
    bridge
        .send_proof(player, Method::Delete, "/v1/discord", body)
        .await
}

/// BluMint's player lookup for these Discord users.
async fn lookup(bridge: &Bridge, provider: &str, users: &[&str]) -> Json {
    let (status, found) = bridge
        .post(provider, "/v1/blumint/lookup", json!({ "discord": users }))
        .await;
    assert_eq!(status, StatusCode::OK, "{found}");
    found
}

/// The active links on BluMint's connection: Ember ID and the claim that
/// approved it, by Ember ID.
async fn platform_links(bridge: &Bridge) -> Vec<(String, String)> {
    bridge
        .state()
        .db
        .read(|tx| {
            Ok(tx
                .prepare(
                    "SELECT ember_id, claim_id FROM links
                     WHERE connection_id = ?1 AND revoked_at IS NULL ORDER BY ember_id",
                )?
                .query_map([BLUMINT], |row| Ok((row.get(0)?, row.get(1)?)))?
                .collect::<rusqlite::Result<_>>()?)
        })
        .await
        .unwrap()
}

#[tokio::test]
async fn a_player_connects_discord_once_per_sign_in() {
    let bridge = bridge_with_discord().await;
    let (status, capabilities) = bridge.get("", "/v1/capabilities").await;
    assert_eq!(status, StatusCode::OK);
    assert!(
        capabilities["features"]
            .as_array()
            .unwrap()
            .contains(&json!("discord"))
    );
    let kate = player(&bridge, 1).await;
    assert_eq!(account(&bridge, &kate).await, Json::Null);

    let state = start(&bridge, &kate).await;
    // A state Discord never issued, or a refused code, connects nothing.
    assert!(
        back(&bridge, "code=good-kate&state=made-up")
            .await
            .contains("Sign-in expired")
    );
    let page = back(&bridge, &format!("code=good-kate&state={state}")).await;
    assert!(
        page.contains("Discord connected") && page.contains("kate"),
        "{page}"
    );
    assert!(page.contains(&kate.id().fingerprint()));
    let connected = account(&bridge, &kate).await;
    assert_eq!(
        (
            connected["user_id"].as_str(),
            connected["username"].as_str()
        ),
        (Some(KATE), Some("kate"))
    );
    // Once only.
    assert!(
        back(&bridge, &format!("code=good-kate&state={state}"))
            .await
            .contains("Sign-in expired")
    );

    // Cancelled on Discord, or a code Discord refuses: nothing changes.
    let state = start(&bridge, &kate).await;
    assert!(
        back(&bridge, &format!("error=access_denied&state={state}"))
            .await
            .contains("Not connected")
    );
    let state = start(&bridge, &kate).await;
    assert!(
        back(&bridge, &format!("code=bad&state={state}"))
            .await
            .contains("Discord did not answer")
    );
    assert_eq!(account(&bridge, &kate).await["user_id"], KATE);

    // A sign-in lapses after ten minutes.
    let state = start(&bridge, &kate).await;
    bridge.clock.advance(10 * 60 + 1);
    assert!(
        back(&bridge, &format!("code=good-sam&state={state}"))
            .await
            .contains("Sign-in expired")
    );
}

#[tokio::test]
async fn the_latest_sign_in_wins_and_a_player_can_disconnect() {
    let bridge = bridge_with_discord().await;
    let (one, two) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let state = start(&bridge, &one).await;
    back(&bridge, &format!("code=good-kate&state={state}")).await;
    // The same Discord account signed in from another Ember ID moves there
    // once the player says so.
    move_here(&bridge, &two, "kate").await;
    assert_eq!(account(&bridge, &one).await, Json::Null);
    assert_eq!(account(&bridge, &two).await["user_id"], KATE);
    // Another account replaces this Ember ID's.
    let state = start(&bridge, &two).await;
    back(&bridge, &format!("code=good-sam&state={state}")).await;
    assert_eq!(account(&bridge, &two).await["user_id"], SAM);

    // Disconnecting needs a proof made for it: no other action is issued for that route.
    let refused = bridge
        .client
        .post(bridge.url("/v1/auth/challenges"))
        .bearer_auth(two.token())
        .header("content-type", "application/json")
        .body(
            json!({
                "public_key": two.identity.public_key(),
                "action": Action::DiscordConnect,
                "method": Method::Delete,
                "path": "/v1/discord",
                "request_digest": "0".repeat(43),
            })
            .to_string(),
        )
        .send()
        .await
        .unwrap();
    assert_eq!(refused.status(), StatusCode::BAD_REQUEST);
    let (status, removed) = disconnect(&bridge, &two).await;
    assert_eq!(
        (status, removed["account"].clone()),
        (StatusCode::OK, Json::Null)
    );
    assert_eq!(account(&bridge, &two).await, Json::Null);
}

#[tokio::test]
async fn a_second_answer_for_a_sign_in_is_turned_away() {
    let bridge = bridge_with_discord().await;
    let kate = player(&bridge, 1).await;
    let state = start(&bridge, &kate).await;
    let url = bridge.url(&format!(
        "/v1/discord/callback?code=held-kate&state={state}"
    ));
    let client = bridge.client.clone();
    let first =
        tokio::spawn(async move { client.get(url).send().await.unwrap().text().await.unwrap() });
    HELD.arrived.notified().await;
    // The same answer again, say from a reload, while Discord still checks the first.
    assert!(
        back(&bridge, &format!("code=held-kate&state={state}"))
            .await
            .contains("Sign-in expired")
    );
    HELD.release.notify_one();
    assert!(first.await.unwrap().contains("Discord connected"));
    assert_eq!(account(&bridge, &kate).await["user_id"], KATE);
}

#[tokio::test]
async fn a_disconnect_ends_sign_ins_in_flight() {
    let bridge = bridge_with_discord().await;
    let kate = player(&bridge, 1).await;
    connect(&bridge, &kate, "sam").await;
    // A sign-in the player never finished.
    let state = start(&bridge, &kate).await;
    assert_eq!(disconnect(&bridge, &kate).await.0, StatusCode::OK);
    assert!(
        back(&bridge, &format!("code=good-kate&state={state}"))
            .await
            .contains("Sign-in expired")
    );

    // A sign-in Discord is still confirming when the player disconnects.
    connect(&bridge, &kate, "sam").await;
    let state = start(&bridge, &kate).await;
    let url = bridge.url(&format!(
        "/v1/discord/callback?code=slow-kate&state={state}"
    ));
    let client = bridge.client.clone();
    let answer =
        tokio::spawn(async move { client.get(url).send().await.unwrap().text().await.unwrap() });
    SLOW.arrived.notified().await;
    assert_eq!(disconnect(&bridge, &kate).await.0, StatusCode::OK);
    SLOW.release.notify_one();
    assert!(answer.await.unwrap().contains("Sign-in expired"));
    assert_eq!(account(&bridge, &kate).await, Json::Null);
}

#[tokio::test]
async fn a_platforms_links_follow_the_discord_account() {
    let bridge = bridge_with_discord().await;
    let provider = bridge.provider(BLUMINT).await;
    let (one, two) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let (one_id, two_id) = (one.id().to_string(), two.id().to_string());
    let claim = |user: &str| format!("discord:{user}");
    let mut both = vec![(one_id.clone(), claim(KATE)), (two_id.clone(), claim(SAM))];
    both.sort();
    connect(&bridge, &one, "kate").await;
    connect(&bridge, &two, "sam").await;
    assert_eq!(
        lookup(&bridge, &provider, &[KATE, SAM]).await,
        json!({ "discord": [one_id, two_id] })
    );
    assert_eq!(platform_links(&bridge).await, both);

    // Kate's account moves to the second Ember ID, replacing Sam's there: the
    // links both sign-ins approved end at once.
    move_here(&bridge, &two, "kate").await;
    assert_eq!(platform_links(&bridge).await, vec![]);
    assert_eq!(
        lookup(&bridge, &provider, &[KATE, SAM]).await,
        json!({ "discord": [two_id] })
    );
    assert_eq!(
        platform_links(&bridge).await,
        vec![(two_id.clone(), claim(KATE))]
    );
    // The first Ember ID can no longer be put in a match there.
    let (status, refused) = bridge
        .post(
            &provider,
            "/v1/blumint/matches",
            json!({ "teams": [ { "players": [ { "inGameId": one_id } ] }, { "players": [ { "inGameId": two_id } ] } ] }),
        )
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{refused}");

    // Signing in again with the same account keeps its link.
    connect(&bridge, &two, "kate").await;
    assert_eq!(
        platform_links(&bridge).await,
        vec![(two_id.clone(), claim(KATE))]
    );
    // Disconnecting ends it.
    assert_eq!(disconnect(&bridge, &two).await.0, StatusCode::OK);
    assert_eq!(platform_links(&bridge).await, vec![]);
    assert_eq!(
        lookup(&bridge, &provider, &[KATE]).await,
        json!({ "discord": [] })
    );
}

#[tokio::test]
async fn a_link_the_player_removed_stays_removed_until_they_sign_in_again() {
    let bridge = bridge_with_discord().await;
    let provider = bridge.provider(BLUMINT).await;
    let kate = player(&bridge, 1).await;
    let kate_id = kate.id().to_string();
    connect(&bridge, &kate, "kate").await;
    lookup(&bridge, &provider, &[KATE]).await;
    let (_, links) = bridge.get(kate.token(), "/v1/links").await;
    let link = links["links"][0]["link_id"].as_str().unwrap().to_owned();
    let path = format!("/v1/links/{link}");
    let body = bridge
        .prove(
            &kate,
            Action::LinkRemove,
            Method::Delete,
            &path,
            json!({ "link_id": link }),
        )
        .await;
    let (status, removed) = bridge.send_proof(&kate, Method::Delete, &path, body).await;
    assert_eq!(status, StatusCode::OK, "{removed}");

    // BluMint's next lookups find nobody, and no match can be made.
    for _ in 0..2 {
        assert_eq!(
            lookup(&bridge, &provider, &[KATE]).await,
            json!({ "discord": [] })
        );
    }
    assert_eq!(platform_links(&bridge).await, vec![]);
    let other = player(&bridge, 2).await;
    let (status, _) = bridge
        .post(
            &provider,
            "/v1/blumint/matches",
            json!({ "teams": [ { "players": [ { "inGameId": kate_id } ] }, { "players": [ { "inGameId": other.id() } ] } ] }),
        )
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);

    // Signing in with Discord again gives the consent again.
    connect(&bridge, &kate, "kate").await;
    assert_eq!(
        lookup(&bridge, &provider, &[KATE]).await,
        json!({ "discord": [kate_id] })
    );
}

#[tokio::test]
async fn a_sign_in_started_before_an_unlink_does_not_undo_it() {
    let bridge = bridge_with_discord().await;
    let provider = bridge.provider(BLUMINT).await;
    let kate = player(&bridge, 1).await;
    let kate_id = kate.id().to_string();
    connect(&bridge, &kate, "kate").await;
    lookup(&bridge, &provider, &[KATE]).await;
    // A sign-in is started, and Discord is still answering it when the
    // player removes BluMint's link.
    let state = start(&bridge, &kate).await;
    let url = bridge.url(&format!(
        "/v1/discord/callback?code=late-kate&state={state}"
    ));
    let client = bridge.client.clone();
    let late =
        tokio::spawn(async move { client.get(url).send().await.unwrap().text().await.unwrap() });
    LATE.arrived.notified().await;
    let (_, links) = bridge.get(kate.token(), "/v1/links").await;
    let link = links["links"][0]["link_id"].as_str().unwrap().to_owned();
    let path = format!("/v1/links/{link}");
    let body = bridge
        .prove(
            &kate,
            Action::LinkRemove,
            Method::Delete,
            &path,
            json!({ "link_id": link }),
        )
        .await;
    assert_eq!(
        bridge
            .send_proof(&kate, Method::Delete, &path, body)
            .await
            .0,
        StatusCode::OK
    );
    for _ in 0..2 {
        assert_eq!(
            lookup(&bridge, &provider, &[KATE]).await,
            json!({ "discord": [] })
        );
    }
    // A sign-in started after the unlink, in the same second, gives the
    // consent again; the older one then finishing changes nothing.
    connect(&bridge, &kate, "kate").await;
    LATE.release.notify_one();
    assert!(late.await.unwrap().contains("Sign-in expired"));
    assert_eq!(
        lookup(&bridge, &provider, &[KATE]).await,
        json!({ "discord": [kate_id] })
    );
}

#[tokio::test]
async fn a_code_link_on_a_discord_named_account_is_kept() {
    let bridge = bridge_with_discord().await;
    let provider = bridge.provider(BLUMINT).await;
    let (one, two) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let one_id = one.id().to_string();
    // The platform named its account after Kate's Discord ID and linked it to
    // the first Ember ID with a code; Kate's Discord account is the second's.
    bridge
        .link(&provider, &one, BLUMINT, &format!("discord:{KATE}"))
        .await;
    connect(&bridge, &two, "kate").await;
    connect(&bridge, &one, "sam").await;
    // Kate is not found, the rest of the lookup is, and the code link stays.
    assert_eq!(
        lookup(&bridge, &provider, &[KATE, SAM]).await,
        json!({ "discord": [one_id] })
    );
    let linked: Vec<String> = platform_links(&bridge)
        .await
        .into_iter()
        .map(|(ember_id, _)| ember_id)
        .collect();
    assert_eq!(linked, vec![one_id]);
}

#[tokio::test]
async fn an_account_can_be_disconnected_after_sign_in_is_turned_off() {
    // Configured for Discord, but its client secret is gone.
    let bridge = bridge_with_discord_secret(Secrets::default()).await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    let features = capabilities["features"].as_array().unwrap();
    assert!(!features.contains(&json!("discord")));
    assert!(features.contains(&json!("discord.accounts")));
    let kate = player(&bridge, 1).await;
    let body = bridge
        .prove(
            &kate,
            Action::DiscordConnect,
            Method::Post,
            "/v1/discord/start",
            json!({}),
        )
        .await;
    let (status, _) = bridge
        .send_proof(&kate, Method::Post, "/v1/discord/start", body)
        .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    // Ending sign-ins in flight needs no offer either.
    assert_eq!(cancel(&bridge, &kate).await.0, StatusCode::OK);

    // An account connected while sign-in was on is still found by BluMint.
    let ember_id = kate.id().to_string();
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, 'kate', 0)",
                rusqlite::params![KATE, ember_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    let provider = bridge.provider(BLUMINT).await;
    assert_eq!(
        lookup(&bridge, &provider, &[KATE]).await,
        json!({ "discord": [kate.id()] })
    );
    // The player sees it and disconnects it; BluMint finds nobody after.
    assert_eq!(account(&bridge, &kate).await["user_id"], KATE);
    assert_eq!(disconnect(&bridge, &kate).await.0, StatusCode::OK);
    assert_eq!(account(&bridge, &kate).await, Json::Null);
    assert_eq!(
        lookup(&bridge, &provider, &[KATE]).await,
        json!({ "discord": [] })
    );
    assert_eq!(platform_links(&bridge).await, vec![]);
}

#[tokio::test]
async fn moving_an_account_from_another_ember_id_asks_first() {
    let bridge = bridge_with_discord().await;
    let provider = bridge.provider(BLUMINT).await;
    let (one, two) = (player(&bridge, 1).await, player(&bridge, 2).await);
    let one_id = one.id().to_string();
    connect(&bridge, &one, "kate").await;
    lookup(&bridge, &provider, &[KATE]).await;
    let kept = vec![(one_id.clone(), format!("discord:{KATE}"))];

    // Signing in from the second Ember ID moves nothing yet: the page names
    // both Ember IDs and what moving ends.
    let state = start(&bridge, &two).await;
    let page = back(&bridge, &format!("code=good-kate&state={state}")).await;
    assert!(
        page.contains("Move this Discord account?")
            && page.contains(&one.id().fingerprint())
            && page.contains(&two.id().fingerprint())
            && page.contains("links it made"),
        "{page}"
    );
    assert_eq!(account(&bridge, &one).await["user_id"], KATE);
    assert_eq!(account(&bridge, &two).await, Json::Null);
    assert_eq!(platform_links(&bridge).await, kept);
    // The same answer from Discord again is turned away while the page waits.
    assert!(
        back(&bridge, &format!("code=good-kate&state={state}"))
            .await
            .contains("Sign-in expired")
    );
    // Keeping it changes nothing and ends the sign-in.
    assert!(
        decide(&bridge, &state, "keep")
            .await
            .contains("Nothing moved")
    );
    assert!(
        decide(&bridge, &state, "move")
            .await
            .contains("Sign-in expired")
    );
    assert_eq!(account(&bridge, &one).await["user_id"], KATE);
    assert_eq!(account(&bridge, &two).await, Json::Null);
    assert_eq!(platform_links(&bridge).await, kept);

    // A state Discord never issued, or no choice, moves nothing.
    let state = start(&bridge, &two).await;
    back(&bridge, &format!("code=good-kate&state={state}")).await;
    assert!(
        decide(&bridge, "made-up", "move")
            .await
            .contains("Sign-in expired")
    );
    assert!(
        decide(&bridge, &state, "maybe")
            .await
            .contains("Sign-in expired")
    );
    assert_eq!(account(&bridge, &one).await["user_id"], KATE);
    // Moving it does what the page said, once.
    let page = decide(&bridge, &state, "move").await;
    assert!(
        page.contains("Discord connected") && page.contains(&two.id().fingerprint()),
        "{page}"
    );
    assert!(
        decide(&bridge, &state, "move")
            .await
            .contains("Sign-in expired")
    );
    assert_eq!(account(&bridge, &one).await, Json::Null);
    assert_eq!(account(&bridge, &two).await["user_id"], KATE);
    assert_eq!(platform_links(&bridge).await, vec![]);
    assert_eq!(
        lookup(&bridge, &provider, &[KATE]).await,
        json!({ "discord": [two.id()] })
    );
}

#[tokio::test]
async fn a_move_waiting_for_its_answer_can_end() {
    let bridge = bridge_with_discord().await;
    let (one, two) = (player(&bridge, 1).await, player(&bridge, 2).await);
    connect(&bridge, &one, "kate").await;
    // A newer sign-in, Cancel in Ember, a disconnect, or ten minutes.
    for ending in 0..4 {
        let state = start(&bridge, &two).await;
        let page = back(&bridge, &format!("code=good-kate&state={state}")).await;
        assert!(page.contains("Move this Discord account?"), "{page}");
        match ending {
            0 => {
                start(&bridge, &two).await;
            }
            1 => assert_eq!(cancel(&bridge, &two).await.0, StatusCode::OK),
            2 => assert_eq!(disconnect(&bridge, &two).await.0, StatusCode::OK),
            _ => bridge.clock.advance(10 * 60 + 1),
        }
        assert!(
            decide(&bridge, &state, "move")
                .await
                .contains("Sign-in expired"),
            "ending {ending}"
        );
        // Read as stored: ten minutes on, the players' sessions have lapsed too.
        let owners: Vec<(String, String)> = bridge
            .state()
            .db
            .read(|tx| {
                Ok(tx
                    .prepare("SELECT user_id, ember_id FROM discord_accounts")?
                    .query_map([], |row| Ok((row.get(0)?, row.get(1)?)))?
                    .collect::<rusqlite::Result<_>>()?)
            })
            .await
            .unwrap();
        assert_eq!(owners, vec![(KATE.to_string(), one.id().to_string())]);
    }
}

#[tokio::test]
async fn only_the_latest_sign_in_connects_and_ember_can_end_it() {
    let bridge = bridge_with_discord().await;
    let kate = player(&bridge, 1).await;
    // A newer sign-in ends the older one: a page left open connects nothing.
    let old = start(&bridge, &kate).await;
    let new = start(&bridge, &kate).await;
    assert!(
        back(&bridge, &format!("code=good-sam&state={old}"))
            .await
            .contains("Sign-in expired")
    );
    connect_with(&bridge, &new, "kate").await;
    // Cancelled from Ember: the page then connects nothing, and the account
    // stays as it was.
    let state = start(&bridge, &kate).await;
    assert_eq!(cancel(&bridge, &kate).await.0, StatusCode::OK);
    assert!(
        back(&bridge, &format!("code=good-sam&state={state}"))
            .await
            .contains("Sign-in expired")
    );
    assert_eq!(account(&bridge, &kate).await["user_id"], KATE);
    // Cancelling needs a proof made for it.
    let refused = bridge
        .client
        .post(bridge.url("/v1/auth/challenges"))
        .bearer_auth(kate.token())
        .header("content-type", "application/json")
        .body(
            json!({
                "public_key": kate.identity.public_key(),
                "action": Action::DiscordConnect,
                "method": Method::Delete,
                "path": "/v1/discord/start",
                "request_digest": "0".repeat(43),
            })
            .to_string(),
        )
        .send()
        .await
        .unwrap();
    assert_eq!(refused.status(), StatusCode::BAD_REQUEST);
}

/// Finishes the sign-in `state` as `who`, whose account no other Ember ID has.
async fn connect_with(bridge: &Bridge, state: &str, who: &str) {
    let page = back(bridge, &format!("code=good-{who}&state={state}")).await;
    assert!(page.contains("Discord connected"), "{page}");
}

#[tokio::test]
async fn a_bridge_without_discord_offers_no_sign_in() {
    let bridge = Bridge::start().await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    let features = capabilities["features"].as_array().unwrap();
    assert!(!features.contains(&json!("discord")));
    assert!(features.contains(&json!("discord.accounts")));
    let kate = player(&bridge, 1).await;
    assert_eq!(account(&bridge, &kate).await, Json::Null);
    let response = bridge
        .client
        .get(bridge.url("/v1/discord/callback?code=x&state=y"))
        .send()
        .await
        .unwrap();
    assert!(
        response
            .text()
            .await
            .unwrap()
            .contains("Discord sign-in is off")
    );
}
