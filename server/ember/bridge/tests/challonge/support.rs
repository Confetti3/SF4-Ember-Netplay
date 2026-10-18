//! The stand-in Challonge, the bridge configured for it, and the requests the
//! Challonge sign-in tests make.
use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
};

use axum::{
    Router,
    body::Bytes,
    extract::{RawQuery, State},
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

/// A Challonge user: ID, username.
pub type User = (&'static str, &'static str);
pub const KATE: User = ("1001", "Kate_99");
pub const SAM: User = ("1002", "sam");
pub const CLIENT_ID: &str = "ember-test-client";
pub const CLIENT_SECRET: &str = "challonge-test-secret";
/// Finds players by Challonge username only.
pub const BRACKETS: &str = "bracket-bot";
/// Finds players by Discord account only.
pub const DISCORD_ONLY: &str = "discord-bot";
/// Finds players either way.
pub const BOTH: &str = "both-bot";

/// One request the stand-in received.
#[derive(Clone, Debug)]
pub struct Seen {
    pub path: String,
    /// The query string's parameters.
    pub query: HashMap<String, String>,
    /// The form body's.
    pub form: HashMap<String, String>,
    pub headers: HeaderMap,
}

#[derive(Default)]
struct Script {
    /// Access tokens issued, and whose they are.
    tokens: HashMap<String, User>,
    seen: Vec<Seen>,
}

/// A stand-in for Challonge's token and `/me` endpoints. Codes `good-kate`
/// and `good-sam` sign those users in, `held-kate` waits for `release`
/// after saying it `arrived`, `busy` answers 429, `odd-name` signs in a user
/// whose username lookups cannot match, and anything else is refused.
#[derive(Clone, Default)]
pub struct Challonge {
    script: Arc<Mutex<Script>>,
    pub arrived: Arc<Notify>,
    pub release: Arc<Notify>,
}

impl Challonge {
    /// The requests received at `path`, oldest first.
    pub fn seen(&self, path: &str) -> Vec<Seen> {
        self.script
            .lock()
            .unwrap()
            .seen
            .iter()
            .filter(|seen| seen.path == path)
            .cloned()
            .collect()
    }

    pub fn tokens(&self) -> Vec<String> {
        self.script.lock().unwrap().tokens.keys().cloned().collect()
    }

    fn record(&self, path: &str, query: Option<String>, body: &[u8], headers: HeaderMap) -> Seen {
        let parse = |bytes: &[u8]| url::form_urlencoded::parse(bytes).into_owned().collect();
        let seen = Seen {
            path: path.into(),
            query: parse(query.unwrap_or_default().as_bytes()),
            form: parse(body),
            headers,
        };
        self.script.lock().unwrap().seen.push(seen.clone());
        seen
    }
}

type Reply = (AxumStatus, [(header::HeaderName, &'static str); 1], String);

fn reply(status: AxumStatus, body: Json) -> Reply {
    (
        status,
        [(header::CONTENT_TYPE, "application/json")],
        body.to_string(),
    )
}

async fn token(
    State(fake): State<Challonge>,
    RawQuery(query): RawQuery,
    headers: HeaderMap,
    body: Bytes,
) -> Reply {
    let seen = fake.record("/oauth/token", query, &body, headers);
    let form = |key: &str| seen.form.get(key).map(String::as_str);
    let valid = form("grant_type") == Some("authorization_code")
        && form("client_id") == Some(CLIENT_ID)
        && form("client_secret") == Some(CLIENT_SECRET)
        && form("redirect_uri").is_some_and(|uri| uri.ends_with("/v1/challonge/callback"));
    let user = match form("code") {
        Some("good-kate") => KATE,
        Some("good-sam") => SAM,
        Some("odd-name") => ("1003", "has space"),
        Some("held-kate") => {
            fake.arrived.notify_one();
            fake.release.notified().await;
            KATE
        }
        Some("busy") => {
            return reply(
                AxumStatus::TOO_MANY_REQUESTS,
                json!({ "errors": [{ "status": 429 }] }),
            );
        }
        _ => return reply(AxumStatus::BAD_REQUEST, json!({ "error": "invalid_grant" })),
    };
    if !valid {
        return reply(
            AxumStatus::UNAUTHORIZED,
            json!({ "error": "invalid_client" }),
        );
    }
    let mut script = fake.script.lock().unwrap();
    let access = format!("access-token-{}-{}", user.0, script.tokens.len());
    script.tokens.insert(access.clone(), user);
    reply(
        AxumStatus::OK,
        json!({ "access_token": access, "token_type": "Bearer", "expires_in": 604800,
                "refresh_token": "refresh-token", "scope": "me", "created_at": 1 }),
    )
}

async fn me(State(fake): State<Challonge>, headers: HeaderMap) -> Reply {
    fake.record("/v2.1/me.json", None, &[], headers.clone());
    let script = fake.script.lock().unwrap();
    let bearer = headers
        .get("authorization")
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.strip_prefix("Bearer "));
    let v2 = headers
        .get("authorization-type")
        .is_some_and(|value| value == "v2");
    match bearer.and_then(|token| script.tokens.get(token)) {
        // Challonge's IDs are strings here; a number is read too.
        Some(&(id, username)) if v2 => {
            let id = if id == KATE.0 {
                json!(id)
            } else {
                json!(id.parse::<u64>().unwrap())
            };
            reply(
                AxumStatus::OK,
                json!({ "data": { "id": id, "type": "user",
                         "attributes": { "username": username, "email": "x@example.com" } } }),
            )
        }
        _ => reply(
            AxumStatus::UNAUTHORIZED,
            json!({ "errors": [{ "status": 401, "detail": "Unauthorized" }] }),
        ),
    }
}

/// Serves a stand-in Challonge; its base URL.
pub async fn fake_challonge(fake: Challonge) -> String {
    let app = Router::new()
        .route("/oauth/token", post(token))
        .route("/v2.1/me.json", get(me))
        .with_state(fake);
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let base = format!("http://{}", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await });
    base
}

pub fn direct(id: &str, discord_lookup: bool, challonge_lookup: bool) -> config::Connection {
    config::Connection {
        id: id.into(),
        kind: "direct".into(),
        environment: "staging".into(),
        display_name: format!("Direct {id}"),
        enabled: true,
        api_base: None,
        discord_lookup,
        challonge_lookup,
        disputes: None,
        results_url: None,
        rooms: None,
    }
}

/// The integration secrets with Challonge's client secret.
pub fn with_secret() -> Secrets {
    Secrets {
        challonge_client_secret: Some(Zeroizing::new(CLIENT_SECRET.into())),
        ..Default::default()
    }
}

/// A bridge with Challonge sign-in against `fake`, with these other
/// `challonge` settings and integration secrets, and connections that find
/// players by Challonge username (whose tenant is named Cups), by Discord
/// account, or both. The clock is frozen.
pub async fn bridge_with(fake: &Challonge, settings: Json, secrets: Secrets) -> Bridge {
    let api_base = fake_challonge(fake.clone()).await;
    let bridge = Bridge::start_with(
        |config| {
            let mut challonge = json!({ "client_id": CLIENT_ID, "api_base": api_base });
            challonge
                .as_object_mut()
                .unwrap()
                .extend(settings.as_object().unwrap().clone());
            config.challonge = Some(serde_json::from_value(challonge).unwrap());
            config.integration_secrets = Some("unused-in-tests.json".into());
            config.tenants.push(config::Tenant {
                id: "cups".into(),
                name: "Cups".into(),
                connections: vec![
                    direct(BRACKETS, false, true),
                    direct(DISCORD_ONLY, true, false),
                    direct(BOTH, true, true),
                ],
            });
        },
        secrets,
    )
    .await;
    bridge.clock.freeze();
    bridge
}

pub async fn bridge(fake: &Challonge) -> Bridge {
    bridge_with(fake, json!({}), with_secret()).await
}

pub async fn player(bridge: &Bridge, byte: u8) -> Player {
    let mut player = bridge.player(byte);
    bridge.open_session(&mut player).await;
    player
}

/// A proof-carrying request from `player` to a Challonge route.
pub async fn send(
    bridge: &Bridge,
    player: &Player,
    action: Action,
    method: Method,
    path: &str,
    command: Json,
) -> (StatusCode, Json) {
    let body = bridge.prove(player, action, method, path, command).await;
    bridge.send_proof(player, method, path, body).await
}

pub async fn try_start(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    send(
        bridge,
        player,
        Action::ChallongeConnect,
        Method::Post,
        "/v1/challonge/start",
        json!({}),
    )
    .await
}

/// Starts a sign-in; the state Challonge would hand back.
pub async fn start(bridge: &Bridge, player: &Player) -> String {
    let (status, started) = try_start(bridge, player).await;
    assert_eq!(status, StatusCode::CREATED, "{started}");
    let url = url::Url::parse(started["authorize_url"].as_str().unwrap()).unwrap();
    let query: HashMap<_, _> = url.query_pairs().into_owned().collect();
    query["state"].clone()
}

/// What the browser sees when Challonge sends it back.
pub async fn back(bridge: &Bridge, query: &str) -> String {
    let response = bridge
        .client
        .get(bridge.url(&format!("/v1/challonge/callback?{query}")))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    response.text().await.unwrap()
}

/// The confirmation secret on the page that asks before connecting or
/// moving an account.
pub fn confirmation(page: &str) -> String {
    let marker = "name=\"confirmation\" value=\"";
    let start = page.find(marker).expect("the page asks for a confirmation") + marker.len();
    let end = page[start..].find('"').unwrap();
    page[start..start + end].to_string()
}

/// The player's answer on the page that asked.
pub async fn decide(bridge: &Bridge, confirmation: &str, choice: &str) -> String {
    let response = bridge
        .client
        .post(bridge.url("/v1/challonge/callback"))
        .header("content-type", "application/x-www-form-urlencoded")
        .body(format!("confirmation={confirmation}&choice={choice}"))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    response.text().await.unwrap()
}

/// Signs `player` in with code `code`; the first page the browser shows.
pub async fn sign_in(bridge: &Bridge, player: &Player, code: &str) -> String {
    let state = start(bridge, player).await;
    back(bridge, &format!("code={code}&state={state}")).await
}

/// Signs `player` in as `who` (`kate` or `sam`), whose account no other
/// Ember ID has, saying yes when the page asks.
pub async fn connect(bridge: &Bridge, player: &Player, who: &str) {
    let mut page = sign_in(bridge, player, &format!("good-{who}")).await;
    if page.contains("Connect this Challonge account?") {
        page = decide(bridge, &confirmation(&page), "connect").await;
    }
    assert!(page.contains("Challonge connected"), "{page}");
}

pub async fn cancel(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    send(
        bridge,
        player,
        Action::ChallongeCancel,
        Method::Delete,
        "/v1/challonge/start",
        json!({}),
    )
    .await
}

pub async fn disconnect(bridge: &Bridge, player: &Player) -> (StatusCode, Json) {
    send(
        bridge,
        player,
        Action::ChallongeRemove,
        Method::Delete,
        "/v1/challonge",
        json!({}),
    )
    .await
}

/// The Challonge user ID connected to the player's Ember ID, or None.
pub async fn account(bridge: &Bridge, player: &Player) -> Option<String> {
    let (status, body) = bridge.get(player.token(), "/v1/challonge").await;
    assert_eq!(status, StatusCode::OK, "{body}");
    body["account"]["user_id"].as_str().map(str::to_owned)
}

/// A lookup on `connection` for these Challonge usernames and Discord IDs.
pub async fn lookup(
    bridge: &Bridge,
    connection: &str,
    challonge: &[&str],
    discord: &[&str],
) -> (StatusCode, Json) {
    let provider = bridge.provider(connection).await;
    bridge
        .post(
            &provider,
            "/v1/players/lookup",
            json!({ "challonge": challonge, "discord": discord }),
        )
        .await
}

/// The Challonge players a lookup on `connection` found: (username asked,
/// user ID, Ember ID).
pub async fn found(
    bridge: &Bridge,
    connection: &str,
    usernames: &[&str],
) -> Vec<(String, String, String)> {
    let (status, answer) = lookup(bridge, connection, usernames, &[]).await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    answer["challonge"]
        .as_array()
        .map(|players| {
            players
                .iter()
                .map(|player| {
                    (
                        player["username"].as_str().unwrap().to_owned(),
                        player["challonge_user_id"].as_str().unwrap().to_owned(),
                        player["ember_id"].as_str().unwrap().to_owned(),
                    )
                })
                .collect()
        })
        .unwrap_or_default()
}

/// The player's active link on `connection`: how it was approved and its
/// claim.
pub async fn link_of(
    bridge: &Bridge,
    connection: &str,
    player: &Player,
) -> Option<(String, String)> {
    let (connection, ember_id) = (connection.to_owned(), player.id().as_str().to_owned());
    bridge
        .state()
        .db
        .read(move |tx| {
            use rusqlite::OptionalExtension;
            Ok(tx
                .query_row(
                    "SELECT approved_via, claim_id FROM links
                     WHERE connection_id = ?1 AND ember_id = ?2 AND revoked_at IS NULL",
                    [&connection, &ember_id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?)
        })
        .await
        .unwrap()
}

/// Writes the Discord account `connect` would for Discord, as
/// tests/partner.rs does.
pub async fn connect_discord(bridge: &Bridge, user_id: &str, player: &Player) {
    let (user_id, ember_id) = (user_id.to_owned(), player.id().as_str().to_owned());
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, 'kate', 1)",
                [&user_id, &ember_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}
