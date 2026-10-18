//! The stand-in Challonge, the bridge configured for it, and the requests the
//! Challonge sign-in tests make.
use std::{
    collections::{HashMap, VecDeque},
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

use crate::common::{Bridge, Player};

/// A Challonge user: ID, username.
pub type User = (&'static str, &'static str);
pub const KATE: User = ("1001", "Kate_99");
pub const SAM: User = ("1002", "sam");
pub const CLIENT_ID: &str = "ember-test-client";
/// Finds players by Challonge username only.
pub const BRACKETS: &str = "bracket-bot";
/// Finds players by Discord account only.
pub const DISCORD_ONLY: &str = "discord-bot";
/// Finds players either way.
pub const BOTH: &str = "both-bot";

/// What the stand-in's token endpoint answers to one poll.
#[derive(Clone, Copy, Debug)]
pub enum Answer {
    Pending,
    SlowDown,
    Denied,
    ExpiredToken,
    InvalidGrant,
    Unsupported,
    ServerError,
    Approve(User),
}

/// One request the stand-in received.
#[derive(Clone, Debug)]
pub struct Seen {
    pub path: String,
    /// The query string's parameters.
    pub params: HashMap<String, String>,
    /// The form body's.
    pub form: HashMap<String, String>,
    pub headers: HeaderMap,
}

#[derive(Default)]
struct Script {
    issued: usize,
    /// Device code by user code.
    devices: HashMap<String, String>,
    /// Answers still to give, by device code; `Pending` once they run out.
    answers: HashMap<String, VecDeque<Answer>>,
    /// Access tokens issued, by token, and whether `/me` fails for them.
    tokens: HashMap<String, User>,
    me_fails: bool,
    /// The verification page the next device code names.
    page: Option<String>,
    refuse_devices: bool,
    seen: Vec<Seen>,
}

/// A stand-in for Challonge's device authorization, token and `/me`
/// endpoints, scripted per sign-in by the code the player would enter.
#[derive(Clone, Default)]
pub struct Challonge(Arc<Mutex<Script>>);

impl Challonge {
    fn script(&self) -> std::sync::MutexGuard<'_, Script> {
        self.0.lock().unwrap()
    }

    /// The next answers to polls of the sign-in showing `user_code`.
    pub fn answer(&self, user_code: &str, answers: &[Answer]) {
        let mut script = self.script();
        let device = script.devices[user_code].clone();
        script
            .answers
            .entry(device)
            .or_default()
            .extend(answers.iter().copied());
    }

    pub fn device_code(&self, user_code: &str) -> String {
        self.script().devices[user_code].clone()
    }

    /// Access tokens handed out so far.
    pub fn tokens(&self) -> Vec<String> {
        self.script().tokens.keys().cloned().collect()
    }

    pub fn fail_me(&self, fails: bool) {
        self.script().me_fails = fails;
    }

    pub fn refuse_devices(&self, refuses: bool) {
        self.script().refuse_devices = refuses;
    }

    /// The verification page the next device code names.
    pub fn next_page(&self, page: &str) {
        self.script().page = Some(page.into());
    }

    /// The requests received at `path`, oldest first.
    pub fn seen(&self, path: &str) -> Vec<Seen> {
        self.script()
            .seen
            .iter()
            .filter(|seen| seen.path == path)
            .cloned()
            .collect()
    }

    fn record(&self, path: &str, query: Option<String>, body: &[u8], headers: HeaderMap) -> Seen {
        let seen = Seen {
            path: path.into(),
            params: url::form_urlencoded::parse(query.unwrap_or_default().as_bytes())
                .into_owned()
                .collect(),
            form: url::form_urlencoded::parse(body).into_owned().collect(),
            headers,
        };
        self.script().seen.push(seen.clone());
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

async fn authorize_device(
    State(fake): State<Challonge>,
    RawQuery(query): RawQuery,
    headers: HeaderMap,
    body: Bytes,
) -> Reply {
    let seen = fake.record("/v2.1/oauth/authorize_device", query, &body, headers);
    let mut script = fake.script();
    if script.refuse_devices || seen.params.get("client_id").map(String::as_str) != Some(CLIENT_ID)
    {
        return reply(
            AxumStatus::UNAUTHORIZED,
            json!({ "error": "invalid_client" }),
        );
    }
    script.issued += 1;
    let user_code = format!("CODE{:02}", script.issued);
    let device_code = format!("device-secret-{:02}", script.issued);
    script
        .devices
        .insert(user_code.clone(), device_code.clone());
    let page = script
        .page
        .take()
        .unwrap_or_else(|| "https://auth.challonge.com/oauth/device".into());
    reply(
        AxumStatus::OK,
        json!({
            "device_code": device_code,
            "user_code": user_code,
            "verification_uri": page,
            "verification_uri_complete": format!("{page}?user_code={user_code}"),
            "expires_in": 300,
            "interval": 2,
        }),
    )
}

async fn token(
    State(fake): State<Challonge>,
    RawQuery(query): RawQuery,
    headers: HeaderMap,
    body: Bytes,
) -> Reply {
    let seen = fake.record("/oauth/token", query, &body, headers);
    let mut script = fake.script();
    let device = seen.params.get("device_code").cloned().unwrap_or_default();
    let valid = seen.params.get("grant_type").map(String::as_str)
        == Some("urn:ietf:params:oauth:grant-type:device_code")
        && seen.params.get("client_id").map(String::as_str) == Some(CLIENT_ID)
        && script.devices.values().any(|issued| *issued == device);
    if !valid {
        return reply(
            AxumStatus::BAD_REQUEST,
            json!({ "error": "invalid_request" }),
        );
    }
    let answer = script
        .answers
        .get_mut(&device)
        .and_then(VecDeque::pop_front)
        .unwrap_or(Answer::Pending);
    let refused = |error: &str| {
        reply(
            AxumStatus::BAD_REQUEST,
            json!({ "error": error, "error_description": "The authorization server encountered an unexpected condition." }),
        )
    };
    match answer {
        Answer::Pending => refused("authorization_pending"),
        Answer::SlowDown => refused("slow_down"),
        Answer::Denied => refused("access_denied"),
        Answer::ExpiredToken => refused("expired_token"),
        Answer::InvalidGrant => refused("invalid_grant"),
        Answer::Unsupported => refused("unsupported_grant_type"),
        Answer::ServerError => reply(AxumStatus::BAD_GATEWAY, json!({})),
        Answer::Approve(user) => {
            let token = format!("access-token-{}-{}", user.0, script.tokens.len());
            script.tokens.insert(token.clone(), user);
            reply(
                AxumStatus::OK,
                json!({ "access_token": token, "token_type": "Bearer", "expires_in": 604800,
                        "refresh_token": "refresh-token", "scope": "me", "created_at": 1 }),
            )
        }
    }
}

async fn me(State(fake): State<Challonge>, headers: HeaderMap) -> Reply {
    fake.record("/v2.1/me.json", None, &[], headers.clone());
    let script = fake.script();
    let bearer = headers
        .get("authorization")
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.strip_prefix("Bearer "));
    let v2 = headers
        .get("authorization-type")
        .is_some_and(|value| value == "v2");
    match bearer.and_then(|token| script.tokens.get(token)) {
        Some(_) if script.me_fails => reply(AxumStatus::INTERNAL_SERVER_ERROR, json!({})),
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
        .route("/v2.1/oauth/authorize_device", post(authorize_device))
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

/// A bridge with Challonge sign-in against `fake` (or none), with these
/// other `challonge` settings, and connections that find players by
/// Challonge username, by Discord account, or both. The clock is frozen, so a
/// sign-in is polled only when a test moves it on.
pub async fn bridge_with(fake: Option<&Challonge>, settings: Json, secrets: Secrets) -> Bridge {
    let api_base = match fake {
        Some(fake) => Some(fake_challonge(fake.clone()).await),
        None => None,
    };
    let bridge = Bridge::start_with(
        |config| {
            config.challonge = api_base.map(|api_base| {
                let mut challonge = json!({ "client_id": CLIENT_ID, "api_base": api_base });
                challonge
                    .as_object_mut()
                    .unwrap()
                    .extend(settings.as_object().unwrap().clone());
                serde_json::from_value(challonge).unwrap()
            });
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
    bridge_with(Some(fake), json!({}), Secrets::default()).await
}

/// Moves the clock on until every Ember ID reads as unused for a move
/// (`MOVE_IDLE_SECS`). Sessions lapse with it; a player opens a new one.
pub fn go_idle(bridge: &Bridge) {
    bridge
        .clock
        .advance(ember_protocol::challonge::MOVE_IDLE_SECS as i64);
}

/// Signs `player` in as `user`, whom another Ember ID holds: the state the
/// sign-in ends in.
pub async fn sign_in_held_elsewhere(
    bridge: &Bridge,
    fake: &Challonge,
    player: &Player,
    user: User,
) -> Json {
    let user_code = start(bridge, player).await;
    fake.answer(&user_code, &[Answer::Approve(user)]);
    poll_after(bridge, 5).await;
    connection(bridge, player).await["sign_in"].clone()
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

/// Starts a sign-in; the code the game shows.
pub async fn start(bridge: &Bridge, player: &Player) -> String {
    let (status, started) = try_start(bridge, player).await;
    assert_eq!(status, StatusCode::CREATED, "{started}");
    started["user_code"].as_str().unwrap().to_owned()
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

/// The player's choice for a sign-in in `moving`: `move` or `keep`.
pub async fn choose(bridge: &Bridge, player: &Player, choice: &str) -> (StatusCode, Json) {
    send(
        bridge,
        player,
        Action::ChallongeMove,
        Method::Post,
        "/v1/challonge/move",
        json!({ "choice": choice }),
    )
    .await
}

/// `GET /v1/challonge`.
pub async fn connection(bridge: &Bridge, player: &Player) -> Json {
    let (status, body) = bridge.get(player.token(), "/v1/challonge").await;
    assert_eq!(status, StatusCode::OK, "{body}");
    body
}

/// The state of the player's sign-in, or None.
pub async fn sign_in_state(bridge: &Bridge, player: &Player) -> Option<String> {
    connection(bridge, player).await["sign_in"]["state"]
        .as_str()
        .map(str::to_owned)
}

/// The Challonge user ID connected to the player's Ember ID, or None.
pub async fn account(bridge: &Bridge, player: &Player) -> Option<String> {
    connection(bridge, player).await["account"]["user_id"]
        .as_str()
        .map(str::to_owned)
}

/// Moves the clock `seconds` on and polls Challonge once.
pub async fn poll_after(bridge: &Bridge, seconds: i64) {
    bridge.clock.advance(seconds);
    ember_bridge::poll_challonge(bridge.state()).await;
}

/// Signs `player` in as `user`, approved on the first poll.
pub async fn connect(bridge: &Bridge, fake: &Challonge, player: &Player, user: User) {
    let code = start(bridge, player).await;
    fake.answer(&code, &[Answer::Approve(user)]);
    poll_after(bridge, 5).await;
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

/// The Ember ID `user`'s Challonge account is connected to, read without a
/// session, which would count as using that Ember ID.
pub async fn holder(bridge: &Bridge, user: User) -> Option<String> {
    bridge
        .state()
        .db
        .read(move |tx| {
            use rusqlite::OptionalExtension;
            Ok(tx
                .query_row(
                    "SELECT ember_id FROM challonge_accounts WHERE user_id = ?1",
                    [user.0],
                    |row| row.get(0),
                )
                .optional()?)
        })
        .await
        .unwrap()
}
