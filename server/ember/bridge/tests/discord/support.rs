//! The stand-in Discord, the bridge configured for it, and the requests the
//! Discord sign-in tests make.
use axum::{
    Form, Router,
    http::{HeaderMap, StatusCode as AxumStatus, header},
    routing::{get, post},
};
use ember_bridge::{config, integrations::Secrets};
use ember_protocol::challenge::{Action, Method};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use tokio::sync::Notify;
use zeroize::Zeroizing;

use crate::common::{Bridge, Player};

pub const KATE: &str = "274220342558756145";
pub const SAM: &str = "1725265127372152396";
/// A platform that finds players by Discord account.
pub const BLUMINT: &str = "bm-partner";

/// A code the stand-in Discord holds until the test lets it go: `arrived`
/// says Discord has it, `release` lets it sign Kate in. One per test, so
/// tests holding a code run side by side.
pub struct Gate {
    pub arrived: Notify,
    pub release: Notify,
}

impl Gate {
    const fn new() -> Self {
        Self {
            arrived: Notify::const_new(),
            release: Notify::const_new(),
        }
    }
}

pub static SLOW: Gate = Gate::new();
pub static HELD: Gate = Gate::new();
pub static LATE: Gate = Gate::new();

/// A stand-in for Discord's token and user endpoints. Codes `good-kate`,
/// `good-sam` and the gated `slow-kate`, `held-kate` and `late-kate` sign in
/// those users; anything else is refused.
pub async fn fake_discord() -> String {
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

pub async fn bridge_with_discord() -> Bridge {
    bridge_with_discord_secret(Secrets {
        discord_client_secret: Some(Zeroizing::new("test-secret".into())),
        ..Default::default()
    })
    .await
}

/// A bridge configured for Discord and BluMint, with these secrets.
pub async fn bridge_with_discord_secret(secrets: Secrets) -> Bridge {
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
                    discord_lookup: false,
                    disputes: None,
                    results_url: None,
                    rooms: None,
                }],
            });
        },
        secrets,
    )
    .await
}

pub async fn player(bridge: &Bridge, byte: u8) -> Player {
    let mut player = bridge.player(byte);
    bridge.open_session(&mut player).await;
    player
}

/// Starts a sign-in and returns the state Discord would hand back.
pub async fn start(bridge: &Bridge, player: &Player) -> String {
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
pub async fn back(bridge: &Bridge, query: &str) -> String {
    let response = bridge
        .client
        .get(bridge.url(&format!("/v1/discord/callback?{query}")))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    response.text().await.unwrap()
}

pub async fn account(bridge: &Bridge, player: &Player) -> Json {
    let (status, body) = bridge.get(player.token(), "/v1/discord").await;
    assert_eq!(status, StatusCode::OK, "{body}");
    body["account"].clone()
}

/// Signs `player` in as `who` (`kate` or `sam`), whose account no other
/// Ember ID has, answering the page's question when it asks.
pub async fn connect(bridge: &Bridge, player: &Player, who: &str) {
    let state = start(bridge, player).await;
    connect_with(bridge, &state, who).await;
}

/// The page a sign-in ends on once the player says yes to a first
/// connection, or the page itself when it did not ask.
pub async fn answered(bridge: &Bridge, page: String) -> String {
    if !page.contains("Connect this Discord account?") {
        return page;
    }
    decide(bridge, &confirmation(&page), "connect").await
}

/// The confirmation secret on the page that asks before moving an account.
pub fn confirmation(page: &str) -> String {
    let marker = "name=\"confirmation\" value=\"";
    let start = page.find(marker).expect("the page asks for a confirmation") + marker.len();
    let end = page[start..].find('"').unwrap();
    page[start..start + end].to_string()
}

/// The player's answer on the page that asks before moving an account.
pub async fn decide(bridge: &Bridge, confirmation: &str, choice: &str) -> String {
    let response = bridge
        .client
        .post(bridge.url("/v1/discord/callback"))
        .header("content-type", "application/x-www-form-urlencoded")
        .body(format!("confirmation={confirmation}&choice={choice}"))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    response.text().await.unwrap()
}

/// Signs `player` in as `who`, whose account another Ember ID has, and moves it.
pub async fn move_here(bridge: &Bridge, player: &Player, who: &str) {
    let state = start(bridge, player).await;
    let page = back(bridge, &format!("code=good-{who}&state={state}")).await;
    assert!(page.contains("Move this Discord account?"), "{page}");
    let page = decide(bridge, &confirmation(&page), "move").await;
    assert!(page.contains("Discord connected"), "{page}");
}

/// Ends the player's sign-ins in flight, as Ember's Cancel does.
pub async fn cancel(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
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

pub async fn disconnect(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
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
pub async fn lookup(bridge: &Bridge, provider: &str, users: &[&str]) -> Json {
    let (status, found) = bridge
        .post(provider, "/v1/blumint/lookup", json!({ "discord": users }))
        .await;
    assert_eq!(status, StatusCode::OK, "{found}");
    found
}

/// The active links on BluMint's connection: Ember ID and the claim that
/// approved it, by Ember ID.
pub async fn platform_links(bridge: &Bridge) -> Vec<(String, String)> {
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

/// Finishes the sign-in `state` as `who`, whose account no other Ember ID has.
pub async fn connect_with(bridge: &Bridge, state: &str, who: &str) {
    let page = back(bridge, &format!("code=good-{who}&state={state}")).await;
    let page = answered(bridge, page).await;
    assert!(page.contains("Discord connected"), "{page}");
}
