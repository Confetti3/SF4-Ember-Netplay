//! The tournament desk on loopback ports for a browser to drive
//! (scripts/e2e-desk.mjs): a real bridge and connector with the fake
//! Challonge, a stand-in Discord webhook, an organizer holding organizer
//! access, and four players. It serves until killed; ignored by default.
//!
//! ```sh
//! cargo test -p ember-brackets --test desk_e2e_stack -- --ignored --nocapture
//! ```
//!
//! It writes `stack.json` (bridge origin, the webhook's URL, the organizer's
//! seed file, the control address) to `EMBER_E2E_OUT` (default the temp
//! folder). The organizer's game side is `examples/desk_game.rs` with that
//! seed. The control address stands in for the players and Challonge:
//! `POST /join/<tournament>` enters the four players (the desk starts it),
//! `POST /dispute` has the first set's players report game 1 both ways,
//! `POST /pass` lets the connector act, `GET /posts` lists the Discord posts.
#[path = "../../bridge/tests/common/mod.rs"]
mod common;
mod support;

use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
};

use axum::{
    Router,
    body::Bytes,
    extract::{Path, State},
    routing::{get, post},
};
use common::{
    Bridge, Player,
    play::{self, Table},
    sign_in::{self, definition},
    sites::{self, TestSite},
};
use ember_brackets::{Clock, Connector, Desk};
use ember_bridge::config::{Connection, Disputes};
use ember_protocol::report::Outcome::{P1Win, P2Win};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use support::{
    CONNECTION,
    challonge::{self, FakeChallonge},
    oauth::approve,
};

const CHALLONGE: TestSite = TestSite("challonge");
const ORGANIZER: (&str, &str) = ("9009", "organizer");

struct Stack {
    bridge: Bridge,
    connector: Connector,
    challonge: FakeChallonge,
    players: Mutex<HashMap<String, Player>>,
    posts: Arc<Mutex<Vec<Json>>>,
}

struct Seats<'a>(&'a Bridge, &'a Player, &'a Player);

impl Table for Seats<'_> {
    fn bridge(&self) -> &Bridge {
        self.0
    }
    fn a(&self) -> &Player {
        self.1
    }
    fn b(&self) -> &Player {
        self.2
    }
}

async fn sign_in_at(bridge: &Bridge, site: TestSite, authorize: &url::Url) -> String {
    let back = approve(authorize.as_str()).await;
    let page = site.back(bridge, &back).await;
    if !page.contains("name=\"confirmation\"") {
        return page;
    }
    site.decide(bridge, &sign_in::confirmation(&page), "connect")
        .await
}

impl Stack {
    fn browse_as(&self, user: (&str, &str)) {
        self.challonge.state.lock().unwrap().oauth.browser = Some((user.0.into(), user.1.into()));
    }

    async fn pass(&self) {
        ember_bridge::deliver_results(self.bridge.state()).await;
        self.connector.pass(true).await;
    }

    /// The organizer acting on Challonge with their own token.
    async fn on_challonge(&self, method: reqwest::Method, path: &str, body: Json) -> StatusCode {
        let token = self
            .challonge
            .state
            .lock()
            .unwrap()
            .oauth
            .token_of(ORGANIZER.0, "tournaments:write");
        self.bridge
            .client
            .request(method, format!("{}/v2.1/{path}", self.challonge.api))
            .header("authorization-type", "v2")
            .bearer_auth(token)
            .header("content-type", "application/vnd.api+json")
            .header("accept", "application/json")
            .body(body.to_string())
            .send()
            .await
            .unwrap()
            .status()
    }

    /// Four players join the tournament; the organizer starts it from the
    /// desk.
    async fn join(&self, tournament: &str) -> Json {
        for byte in 1..=4 {
            self.on_challonge(
                reqwest::Method::POST,
                &format!("tournaments/{tournament}/participants.json"),
                json!({ "data": { "type": "Participants", "attributes": {
                    "name": format!("Player {byte}"), "username": format!("player{byte}") } } }),
            )
            .await;
        }
        json!({ "joined": 4 })
    }

    /// The first Ember match's players each report game 1 as their own win.
    async fn dispute(&self) -> Json {
        let match_id = self.bridge.state().db.read(|tx| {
            Ok(tx.query_row(
                "SELECT id FROM matches ORDER BY created_at, rowid LIMIT 1",
                [],
                |row| row.get::<_, String>(0),
            )?)
        });
        let Ok(match_id) = match_id.await else {
            return json!({ "error": "no match yet: start the tournament and mark it ready" });
        };
        let provider = self.bridge.provider(CONNECTION).await;
        let (_, found) = self
            .bridge
            .get(&provider, &format!("/v1/matches/{match_id}"))
            .await;
        let seated: Vec<String> = [0, 1]
            .map(|slot| {
                let participants = found["participants"].as_array().unwrap();
                let player = participants.iter().find(|player| player["slot"] == slot);
                player.unwrap()["ember_id"].as_str().unwrap().to_owned()
            })
            .into();
        let mut players = std::mem::take(&mut *self.players.lock().unwrap());
        for id in &seated {
            self.bridge.open_session(players.get_mut(id).unwrap()).await;
        }
        let seats = Seats(&self.bridge, &players[&seated[0]], &players[&seated[1]]);
        let binding = play::bound(&seats, &match_id).await;
        let permit = play::permit(&seats, &match_id, &binding, 1).await;
        for (n, (player, outcome)) in [(seats.1, P1Win), (seats.2, P2Win)].into_iter().enumerate() {
            play::report(&seats, player, player, &match_id, &permit, n as u8, outcome).await;
        }
        *self.players.lock().unwrap() = players;
        self.pass().await;
        json!({ "match_id": match_id })
    }
}

/// A JSON answer, without axum's json feature.
fn reply(value: Json) -> ([(axum::http::header::HeaderName, &'static str); 1], String) {
    (
        [(axum::http::header::CONTENT_TYPE, "application/json")],
        value.to_string(),
    )
}

#[tokio::test]
#[ignore = "serves a desk for a browser until killed"]
async fn serve_a_desk_for_the_browser() {
    let challonge = FakeChallonge::start(
        ember_brackets::CredentialKind::Bearer,
        Vec::new(),
        Vec::new(),
    )
    .await;
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let results_url = format!("http://{}/results", listener.local_addr().unwrap());
    let mut site = definition("challonge", &challonge.api, challonge::CLIENT_ID);
    site.limits = None;
    let mut secrets = sites::secrets(&[("challonge", challonge::CLIENT_SECRET)]);
    secrets.result_secrets = [(CONNECTION.to_owned(), support::result_secret())].into();
    let connection = Connection {
        disputes: Some(Disputes::Review),
        results_url: Some(results_url),
        ..sites::direct(CONNECTION, false, &["challonge"])
    };
    let bridge =
        sites::bridge_with_desk(vec![site], secrets, vec![connection], Some(CONNECTION)).await;
    let dir = support::temp("e2e");
    let provider = bridge.provider(CONNECTION).await;
    let mut config = support::config(&dir, &bridge.origin, &provider);
    config.bridge.build_id = play::BUILD.into();
    config.monthly_request_budget = 10_000;
    config.desk = Some(Desk {
        api_bases: [("challonge".into(), challonge.api.clone())].into(),
        ..Desk::default()
    });
    let connector = support::connector(&config, &Clock::default());
    connector.serve(listener).unwrap();

    // The organizer (seed byte 9, written for desk_game) links Challonge and
    // allows organizer access; four players link theirs.
    let organizer = sign_in::player(&bridge, 9).await;
    let mut stack = Stack {
        bridge,
        connector,
        challonge,
        players: Mutex::default(),
        posts: Arc::default(),
    };
    stack.browse_as(ORGANIZER);
    let authorize = CHALLONGE.start_organizing(&stack.bridge, &organizer).await;
    sign_in_at(&stack.bridge, CHALLONGE, &authorize).await;
    for byte in 1..=4u8 {
        let player = sign_in::player(&stack.bridge, byte).await;
        let user = (format!("10{byte}"), format!("player{byte}"));
        stack.browse_as((&user.0, &user.1));
        let authorize =
            sign_in::start(&stack.bridge, &player, &sign_in::Routes::site("challonge")).await;
        sign_in_at(&stack.bridge, CHALLONGE, &authorize).await;
        stack
            .players
            .get_mut()
            .unwrap()
            .insert(player.id().as_str().to_owned(), player);
    }

    // A stand-in Discord webhook, on the control port too.
    let out = std::env::var("EMBER_E2E_OUT").map_or_else(|_| std::env::temp_dir(), Into::into);
    std::fs::create_dir_all(&out).unwrap();
    let seed = out.join("organizer.seed");
    std::fs::write(&seed, [9u8; 32]).unwrap();
    let control = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let control_url = format!("http://{}", control.local_addr().unwrap());
    let stack = Arc::new(stack);
    let app = Router::new()
        .route(
            "/api/webhooks/{id}/{token}",
            post(|State(stack): State<Arc<Stack>>, body: Bytes| async move {
                stack
                    .posts
                    .lock()
                    .unwrap()
                    .push(serde_json::from_slice(&body).unwrap_or(Json::Null));
                StatusCode::NO_CONTENT
            }),
        )
        .route(
            "/posts",
            get(|State(stack): State<Arc<Stack>>| async move {
                reply(Json::from(stack.posts.lock().unwrap().clone()))
            }),
        )
        .route(
            "/pass",
            post(|State(stack): State<Arc<Stack>>| async move {
                stack.pass().await;
                reply(json!({}))
            }),
        )
        .route(
            "/join/{tournament}",
            post(
                |State(stack): State<Arc<Stack>>, Path(tournament): Path<String>| async move {
                    reply(stack.join(&tournament).await)
                },
            ),
        )
        .route(
            "/dispute",
            post(|State(stack): State<Arc<Stack>>| async move { reply(stack.dispute().await) }),
        )
        .with_state(stack.clone());
    let info = json!({
        "bridge": stack.bridge.origin,
        "webhook": format!("{control_url}/api/webhooks/123456789/e2e-token"),
        "seed": seed,
        "control": control_url,
    });
    std::fs::write(out.join("stack.json"), info.to_string()).unwrap();
    println!("desk e2e stack: {info}");
    axum::serve(control, app).await.unwrap();
}
