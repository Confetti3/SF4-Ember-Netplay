//! Signing in with another site (Discord, or a configured sign-in site) the
//! way the tests do it: a stand-in for the site's token and user endpoints
//! (`FakeSite`), a site definition from `deploy/sites/` pointed at it
//! (`definition`), and the requests a player's Ember and browser make to the
//! bridge, on whichever site's routes (`Routes`).
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
use ember_bridge::config::SignInSite;
use ember_protocol::{
    challenge::{Action, Method},
    discord, sites,
};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use tokio::sync::Notify;

use super::{Bridge, Player};

/// A user on the stand-in site: ID and username.
pub type User = (&'static str, &'static str);

/// A code the stand-in holds until the test lets it go: `arrived` says the
/// site has it, `release` lets it answer. One per test, so tests holding a
/// code run side by side.
pub struct Gate {
    pub arrived: Notify,
    pub release: Notify,
}

impl Gate {
    pub const fn new() -> Self {
        Self {
            arrived: Notify::const_new(),
            release: Notify::const_new(),
        }
    }
}

/// What the stand-in's token endpoint does with a code.
pub enum Code {
    SignsIn(User),
    /// Signs the user in once the test releases the gate.
    Held(&'static Gate, User),
    /// Answers this status and nothing the bridge can use.
    Fails(u16),
}

/// One request the stand-in received.
#[derive(Clone, Debug)]
pub struct Seen {
    pub path: String,
    /// The query string's parameters.
    pub query: HashMap<String, String>,
    /// The body's fields: a form's, or a JSON object's string fields.
    pub form: HashMap<String, String>,
    /// The body, as text.
    pub body: String,
    pub headers: HeaderMap,
}

/// A stand-in OAuth site: it exchanges the codes it knows for a token when
/// the request names its client, and answers its user endpoint for that
/// token with `me_body`, if the request carries `me_headers`. It records
/// every request.
pub struct FakeSite {
    pub token_path: &'static str,
    pub me_path: &'static str,
    pub client_id: &'static str,
    pub client_secret: &'static str,
    /// The end of the redirect URI the bridge must send.
    pub callback: &'static str,
    pub codes: Vec<(&'static str, Code)>,
    pub me_headers: &'static [(&'static str, &'static str)],
    pub me_body: fn(User) -> Json,
    seen: Mutex<Vec<Seen>>,
    tokens: Mutex<HashMap<String, User>>,
    refreshes: Mutex<HashMap<String, User>>,
}

type Reply = (AxumStatus, [(header::HeaderName, &'static str); 1], String);

fn reply(status: AxumStatus, body: Json) -> Reply {
    (
        status,
        [(header::CONTENT_TYPE, "application/json")],
        body.to_string(),
    )
}

impl FakeSite {
    pub fn new(
        token_path: &'static str,
        me_path: &'static str,
        client: (&'static str, &'static str),
        callback: &'static str,
        codes: Vec<(&'static str, Code)>,
        me_headers: &'static [(&'static str, &'static str)],
        me_body: fn(User) -> Json,
    ) -> Arc<Self> {
        Arc::new(Self {
            token_path,
            me_path,
            client_id: client.0,
            client_secret: client.1,
            callback,
            codes,
            me_headers,
            me_body,
            seen: Mutex::default(),
            tokens: Mutex::default(),
            refreshes: Mutex::default(),
        })
    }

    /// Serves the stand-in on a loopback port; its origin.
    pub async fn serve(self: &Arc<Self>) -> String {
        let app = Router::new()
            .route(self.token_path, post(token))
            .route(self.me_path, get(me).post(me))
            .with_state(self.clone());
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let origin = format!("http://{}", listener.local_addr().unwrap());
        tokio::spawn(async move { axum::serve(listener, app).await });
        origin
    }

    /// The requests received at `path`, oldest first.
    pub fn seen(&self, path: &str) -> Vec<Seen> {
        self.seen
            .lock()
            .unwrap()
            .iter()
            .filter(|seen| seen.path == path)
            .cloned()
            .collect()
    }

    /// The access tokens handed out so far.
    pub fn tokens(&self) -> Vec<String> {
        self.tokens.lock().unwrap().keys().cloned().collect()
    }

    fn record(&self, path: &str, query: Option<String>, body: &[u8], headers: HeaderMap) -> Seen {
        let parse = |bytes: &[u8]| url::form_urlencoded::parse(bytes).into_owned().collect();
        let json = headers
            .get("content-type")
            .is_some_and(|value| value == "application/json");
        let form = match serde_json::from_slice::<serde_json::Map<String, Json>>(body) {
            Ok(fields) if json => fields
                .into_iter()
                .filter_map(|(name, value)| Some((name, value.as_str()?.to_owned())))
                .collect(),
            _ => parse(body),
        };
        let seen = Seen {
            path: path.into(),
            query: parse(query.unwrap_or_default().as_bytes()),
            form,
            body: String::from_utf8_lossy(body).into_owned(),
            headers,
        };
        self.seen.lock().unwrap().push(seen.clone());
        seen
    }
}

async fn token(
    State(site): State<Arc<FakeSite>>,
    RawQuery(query): RawQuery,
    headers: HeaderMap,
    body: Bytes,
) -> Reply {
    let seen = site.record(site.token_path, query, &body, headers);
    let form = |key: &str| seen.form.get(key).map(String::as_str);
    // A refresh token it handed out, for the client that holds it.
    if form("grant_type") == Some("refresh_token") {
        let user = form("refresh_token")
            .and_then(|refresh| site.refreshes.lock().unwrap().get(refresh).copied())
            .filter(|_| {
                form("client_id") == Some(site.client_id)
                    && form("client_secret") == Some(site.client_secret)
            });
        return match user {
            Some(user) => site.issue(user),
            None => reply(AxumStatus::BAD_REQUEST, json!({ "error": "invalid_grant" })),
        };
    }
    let code = site
        .codes
        .iter()
        .find(|(code, _)| form("code") == Some(*code))
        .map(|(_, code)| code);
    let user = match code {
        Some(Code::SignsIn(user)) => *user,
        Some(Code::Held(gate, user)) => {
            gate.arrived.notify_one();
            gate.release.notified().await;
            *user
        }
        Some(Code::Fails(status)) => {
            return reply(
                AxumStatus::from_u16(*status).unwrap(),
                json!({ "errors": [{ "status": status }] }),
            );
        }
        None => return reply(AxumStatus::BAD_REQUEST, json!({ "error": "invalid_grant" })),
    };
    let valid = form("grant_type") == Some("authorization_code")
        && form("client_id") == Some(site.client_id)
        && form("client_secret") == Some(site.client_secret)
        && form("redirect_uri").is_some_and(|uri| uri.ends_with(site.callback));
    if !valid {
        return reply(AxumStatus::BAD_REQUEST, json!({ "error": "invalid_grant" }));
    }
    site.issue(user)
}

impl FakeSite {
    /// A new access token and refresh token for `user`.
    fn issue(&self, user: User) -> Reply {
        let mut tokens = self.tokens.lock().unwrap();
        let access = format!("access-token-{}-{}", user.0, tokens.len());
        let refresh = format!("refresh-token-{}-{}", user.0, tokens.len());
        tokens.insert(access.clone(), user);
        self.refreshes.lock().unwrap().insert(refresh.clone(), user);
        reply(
            AxumStatus::OK,
            json!({ "access_token": access, "token_type": "Bearer", "expires_in": 604800,
                    "refresh_token": refresh, "scope": "identify me" }),
        )
    }
}

async fn me(State(site): State<Arc<FakeSite>>, headers: HeaderMap, body: Bytes) -> Reply {
    site.record(site.me_path, None, &body, headers.clone());
    let header = |name: &str| headers.get(name).and_then(|value| value.to_str().ok());
    let user = header("authorization")
        .and_then(|value| value.strip_prefix("Bearer "))
        .and_then(|token| site.tokens.lock().unwrap().get(token).copied());
    let headed = site
        .me_headers
        .iter()
        .all(|(name, value)| header(name) == Some(*value));
    match user {
        Some(user) if headed => reply(AxumStatus::OK, (site.me_body)(user)),
        _ => reply(AxumStatus::UNAUTHORIZED, json!({})),
    }
}

/// A site's sign-in routes on the bridge and the proof actions they take.
pub struct Routes {
    pub start: String,
    pub callback: String,
    pub account: String,
    pub connect: Action,
    pub cancel: Action,
    pub remove: Action,
}

impl Routes {
    pub fn discord() -> Self {
        Self {
            start: discord::START_PATH.into(),
            callback: discord::CALLBACK_PATH.into(),
            account: discord::ACCOUNT_PATH.into(),
            connect: Action::DiscordConnect,
            cancel: Action::DiscordCancel,
            remove: Action::DiscordRemove,
        }
    }

    /// A configured sign-in site's.
    pub fn site(id: &str) -> Self {
        Self {
            start: sites::start_path(id),
            callback: sites::callback_path(id),
            account: sites::account_path(id),
            connect: Action::SiteConnect,
            cancel: Action::SiteCancel,
            remove: Action::SiteRemove,
        }
    }
}

/// The shipped definition `deploy/sites/<name>.json`, as an operator would
/// copy it into `sign_in_sites`, with every URL's origin replaced by
/// `origin` (a stand-in's) and its client ID set to `client_id`.
pub fn definition(name: &str, origin: &str, client_id: &str) -> SignInSite {
    let path = format!("{}/deploy/sites/{name}.json", env!("CARGO_MANIFEST_DIR"));
    let mut site: SignInSite = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
    let local = |url: &str| {
        let url = url::Url::parse(url).unwrap();
        format!("{origin}{}", url.path())
    };
    site.authorize_url = local(&site.authorize_url);
    site.token_url = local(&site.token_url);
    site.user.url = local(&site.user.url);
    if let Some(organizer) = &mut site.organizer {
        organizer.refresh_url = organizer.refresh_url.as_deref().map(local);
    }
    site.client_id = client_id.into();
    site
}

pub async fn player(bridge: &Bridge, byte: u8) -> Player {
    let mut player = bridge.player(byte);
    bridge.open_session(&mut player).await;
    player
}

/// A proof-carrying request from `player`.
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

pub async fn try_start(bridge: &Bridge, player: &Player, site: &Routes) -> (StatusCode, Json) {
    send(
        bridge,
        player,
        site.connect,
        Method::Post,
        &site.start,
        json!({}),
    )
    .await
}

/// Starts a sign-in; the page the browser is sent to.
pub async fn start(bridge: &Bridge, player: &Player, site: &Routes) -> url::Url {
    let (status, started) = try_start(bridge, player, site).await;
    assert_eq!(status, StatusCode::CREATED, "{started}");
    url::Url::parse(started["authorize_url"].as_str().unwrap()).unwrap()
}

/// The parameters of the page a start sends the browser to.
pub fn query(url: &url::Url) -> HashMap<String, String> {
    url.query_pairs().into_owned().collect()
}

/// What the browser sees when the site sends it back.
pub async fn back(bridge: &Bridge, site: &Routes, query: &str) -> String {
    let response = bridge
        .client
        .get(bridge.url(&format!("{}?{query}", site.callback)))
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
pub async fn decide(bridge: &Bridge, site: &Routes, confirmation: &str, choice: &str) -> String {
    let response = bridge
        .client
        .post(bridge.url(&site.callback))
        .header("content-type", "application/x-www-form-urlencoded")
        .body(format!("confirmation={confirmation}&choice={choice}"))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    response.text().await.unwrap()
}

/// Ends the player's sign-ins in flight, as Ember's Cancel does.
pub async fn cancel(bridge: &Bridge, player: &Player, site: &Routes) -> (StatusCode, Json) {
    send(
        bridge,
        player,
        site.cancel,
        Method::Delete,
        &site.start,
        json!({}),
    )
    .await
}

pub async fn disconnect(bridge: &Bridge, player: &Player, site: &Routes) -> (StatusCode, Json) {
    send(
        bridge,
        player,
        site.remove,
        Method::Delete,
        &site.account,
        json!({}),
    )
    .await
}

/// The player's connected account, or null.
pub async fn account(bridge: &Bridge, player: &Player, site: &Routes) -> Json {
    let (status, body) = bridge.get(player.token(), &site.account).await;
    assert_eq!(status, StatusCode::OK, "{body}");
    body["account"].clone()
}
