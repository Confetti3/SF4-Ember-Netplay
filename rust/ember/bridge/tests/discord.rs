//! Discord sign-in against a stand-in Discord: starting needs a proof, the
//! callback takes each sign-in once and in time, the latest sign-in wins, and
//! a bridge without Discord offers none of it.
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
use zeroize::Zeroizing;

const KATE: &str = "274220342558756145";
const SAM: &str = "1725265127372152396";

/// A stand-in for Discord's token and user endpoints. Codes `good-kate` and
/// `good-sam` sign in those users; anything else is refused.
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
        match form.get("code").map(String::as_str) {
            Some(code @ ("good-kate" | "good-sam")) if valid => answer(
                AxumStatus::OK,
                json!({ "access_token": format!("token-{}", &code[5..]), "token_type": "Bearer", "scope": "identify" }),
            ),
            _ => answer(AxumStatus::BAD_REQUEST, json!({ "error": "invalid_grant" })),
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
    let api_base = fake_discord().await;
    Bridge::start_with(
        |config| {
            config.discord = Some(config::Discord {
                client_id: "1546980049692135514".into(),
                api_base,
            });
            config.integration_secrets = Some("unused-in-tests.json".into());
        },
        Secrets {
            discord_client_secret: Some(Zeroizing::new("test-secret".into())),
            ..Default::default()
        },
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
    // The same Discord account signed in from another Ember ID moves there.
    let state = start(&bridge, &two).await;
    back(&bridge, &format!("code=good-kate&state={state}")).await;
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
    let body = bridge
        .prove(
            &two,
            Action::DiscordRemove,
            Method::Delete,
            "/v1/discord",
            json!({}),
        )
        .await;
    let (status, removed) = bridge
        .send_proof(&two, Method::Delete, "/v1/discord", body)
        .await;
    assert_eq!(
        (status, removed["account"].clone()),
        (StatusCode::OK, Json::Null)
    );
    assert_eq!(account(&bridge, &two).await, Json::Null);
}

#[tokio::test]
async fn a_bridge_without_discord_offers_none_of_it() {
    let bridge = Bridge::start().await;
    let (_, capabilities) = bridge.get("", "/v1/capabilities").await;
    assert!(
        !capabilities["features"]
            .as_array()
            .unwrap()
            .contains(&json!("discord"))
    );
    let kate = player(&bridge, 1).await;
    let (status, _) = bridge.get(kate.token(), "/v1/discord").await;
    assert_eq!(status, StatusCode::NOT_FOUND);
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
