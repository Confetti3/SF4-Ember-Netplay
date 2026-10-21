//! A desk event on the real bracket site (server/bracket-site), end to end:
//! the organizer signs in to the site through the bridge (the site signs
//! them in with a stand-in Discord), creates the event from the desk, opens
//! sign-ups with the site's own moves, and three players with Discord
//! connected in Ember sign up through Ember with no site account linked.
//! The organizer starts it; the connector runs it on the `http` adapter with
//! the organizer's token, the players play each set in Ember, and the site
//! finishes the bracket. Needs Node 22.18 or later with TypeScript support
//! on PATH, so it is ignored by default:
//!
//! ```sh
//! cargo test -p ember-brackets --test bracket_site_desk -- --ignored
//! ```
#[path = "../../bridge/tests/common/mod.rs"]
mod common;
mod support;

use std::{
    path::Path,
    process::{Child, Command},
    time::Duration,
};

use axum::{
    Router,
    body::Bytes,
    http::{HeaderMap, header},
    response::IntoResponse,
    routing::{get, post},
};
use common::{
    Bridge, Player,
    play::{self, Table},
    sign_in::{self, definition},
    sites::{self, TestSite},
};
use ember_brackets::{Clock, Desk};
use ember_bridge::config::Connection;
use ember_protocol::{
    challenge::{Action, Method},
    report::Outcome::{self, P1Win, P2Win},
};
use reqwest::{StatusCode, redirect::Policy};
use serde_json::{Value as Json, json};
use support::CONNECTION;

const SITE: TestSite = TestSite("ember-bracket-site");
const CLIENT_ID: &str = "ember-bridge";
const CLIENT_SECRET: &str = "bracket-site-client-secret";
const ORGANIZER_DISCORD: &str = "274220342558756140";

/// Discord, signing in whoever a code names: `code-<user ID>`.
async fn discord() -> String {
    let app = Router::new()
        .route(
            "/api/oauth2/token",
            post(|body: Bytes| async move {
                let form = support::oauth::params(&body);
                let user = form["code"].strip_prefix("code-").unwrap_or_default();
                answer(json!({ "access_token": format!("at-{user}"), "token_type": "Bearer" }))
            }),
        )
        .route(
            "/api/users/@me",
            get(|headers: HeaderMap| async move {
                let token = headers["authorization"].to_str().unwrap();
                let user = token.strip_prefix("Bearer at-").unwrap().to_owned();
                answer(json!({ "id": user, "username": format!("user{}", &user[16..]) }))
            }),
        );
    support::serve(app).await
}

fn answer(body: Json) -> impl IntoResponse {
    (
        [(header::CONTENT_TYPE, "application/json")],
        body.to_string(),
    )
}

/// The bracket site, as `node src/main.ts` runs it.
struct Site(Child);

impl Drop for Site {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

async fn site(dir: &Path, port: u16, bridge: &Bridge, discord: &str) -> Site {
    let write = |name: &str, text: &str| {
        let path = dir.join(name);
        std::fs::write(&path, text).unwrap();
        path
    };
    let main = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../bracket-site/src/main.ts");
    let origin = format!("http://127.0.0.1:{port}");
    let child = Command::new("node")
        .arg(main)
        .env("BRACKET_SITE_LISTEN", format!("127.0.0.1:{port}"))
        .env("BRACKET_SITE_ORIGIN", &origin)
        .env("BRACKET_SITE_DATABASE", dir.join("site.sqlite3"))
        .env(
            "BRACKET_SITE_OPERATOR_TOKEN_FILE",
            write("operator", "operator-token"),
        )
        .env(
            "BRACKET_SITE_CONNECTOR_TOKEN_FILE",
            write("connector", "connector-token"),
        )
        .env("DISCORD_CLIENT_ID", "1546980049692135514")
        .env(
            "DISCORD_CLIENT_SECRET_FILE",
            write("discord", "discord-secret"),
        )
        .env("DISCORD_API_BASE", format!("{discord}/api"))
        .env(
            "DISCORD_AUTHORIZE_URL",
            format!("{discord}/oauth2/authorize"),
        )
        .env("BRACKET_SITE_BRIDGE_CLIENT_ID", CLIENT_ID)
        .env(
            "BRACKET_SITE_BRIDGE_CLIENT_SECRET_FILE",
            write("client", CLIENT_SECRET),
        )
        .env(
            "BRACKET_SITE_BRIDGE_REDIRECT_URI",
            format!("{}/v1/sites/{}/callback", bridge.origin, SITE.0),
        )
        .env("EMBER_BRIDGE_ORIGIN", &bridge.origin)
        .env(
            "EMBER_BRIDGE_CREDENTIAL_FILE",
            write("provider", &bridge.provider(CONNECTION).await),
        )
        .spawn()
        .expect("node is on PATH");
    let site = Site(child);
    for _ in 0..100 {
        if reqwest::get(format!("{origin}/t/none")).await.is_ok() {
            return site;
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    panic!("the bracket site did not start");
}

/// A set's two players at their table, player 1 first.
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

/// The two players of the bridge's newest match play `games` in Ember, each
/// reporting every one.
async fn play(bridge: &Bridge, players: &mut [Player], games: &[Outcome], generation: &mut u64) {
    let provider = bridge.provider(CONNECTION).await;
    let match_id: String = bridge
        .state()
        .db
        .read(|tx| {
            Ok(tx.query_row(
                "SELECT id FROM matches ORDER BY created_at DESC, rowid DESC",
                [],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap();
    let (_, found) = bridge
        .get(&provider, &format!("/v1/matches/{match_id}"))
        .await;
    let seat = |slot: u64| {
        let participants = found["participants"].as_array().unwrap();
        let id = participants
            .iter()
            .find(|player| player["slot"] == slot)
            .unwrap()["ember_id"]
            .clone();
        players
            .iter()
            .position(|player| player.id().as_str() == id)
            .unwrap()
    };
    let (a, b) = (seat(0), seat(1));
    for index in [a, b] {
        bridge.open_session(&mut players[index]).await;
    }
    let seats = Seats(bridge, &players[a], &players[b]);
    let binding = play::bound(&seats, &match_id).await;
    for (n, &game) in games.iter().enumerate() {
        *generation += 1;
        let permit = play::permit(&seats, &match_id, &binding, *generation).await;
        for player in [seats.1, seats.2] {
            let n = (2 * n + usize::from(player.id() == seats.2.id())) as u8;
            let (status, sent) =
                play::report(&seats, player, player, &match_id, &permit, n, game).await;
            assert_eq!(status, StatusCode::CREATED, "{sent}");
        }
    }
    let (_, played) = bridge
        .get(&provider, &format!("/v1/matches/{match_id}"))
        .await;
    assert_eq!(played["state"], "completed", "{played}");
}

#[tokio::test]
#[ignore = "needs Node with TypeScript support"]
async fn a_desk_event_on_the_bracket_site_is_signed_up_for_and_played_through_ember() {
    let port = std::net::TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port();
    let site_origin = format!("http://127.0.0.1:{port}");
    let discord = discord().await;
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let results_url = format!("http://{}/results", listener.local_addr().unwrap());
    let mut definition = definition(SITE.0, &site_origin, CLIENT_ID);
    definition.limits = None;
    let mut secrets = sites::secrets(&[(SITE.0, CLIENT_SECRET)]);
    secrets.result_secrets = [(CONNECTION.to_owned(), support::result_secret())].into();
    let connection = Connection {
        results_url: Some(results_url),
        ..sites::direct(CONNECTION, true, &[])
    };
    let bridge = sites::bridge_with_desk(
        vec![definition],
        secrets,
        vec![connection],
        Some(CONNECTION),
    )
    .await;
    let dir = support::temp("bracket-site-desk");
    let _site = site(&dir, port, &bridge, &discord).await;

    // The organizer signs in to the site from Ember: the bridge sends the
    // browser to the site, the site on to Discord and back, and the site's
    // code back to the bridge.
    let organizer = sign_in::player(&bridge, 9).await;
    let browser = reqwest::Client::builder()
        .redirect(Policy::none())
        .build()
        .unwrap();
    let location = |response: reqwest::Response| {
        let to = response.headers().get("location");
        let to = to.unwrap_or_else(|| panic!("{} without a location", response.status()));
        url::Url::parse(to.to_str().unwrap()).unwrap()
    };
    let to_discord = location(
        browser
            .get(SITE.start_organizing(&bridge, &organizer).await)
            .send()
            .await
            .unwrap(),
    );
    let state = sign_in::query(&to_discord)["state"].clone();
    let to_bridge = location(
        browser
            .get(format!(
                "{site_origin}/oauth/discord/callback?code=code-{ORGANIZER_DISCORD}&state={state}"
            ))
            .send()
            .await
            .unwrap(),
    );
    let page = SITE.back(&bridge, to_bridge.query().unwrap()).await;
    let page = if page.contains("name=\"confirmation\"") {
        SITE.decide(&bridge, &sign_in::confirmation(&page), "connect")
            .await
    } else {
        page
    };
    assert!(
        SITE.organizer(&bridge, &organizer).await.is_object(),
        "{page}"
    );

    // A draft, created on the site from the desk, with sign-ups through Ember.
    let change = |command: Json, path: String| {
        let (bridge, organizer) = (&bridge, &organizer);
        async move {
            sign_in::send(
                bridge,
                organizer,
                Action::SiteConnect,
                Method::Post,
                &path,
                command,
            )
            .await
        }
    };
    let (_, made) = change(json!({ "title": "Weekly" }), "/v1/organizer/drafts".into()).await;
    let id = made["id"].as_str().unwrap().to_owned();
    let event = |rest: &str| format!("/v1/organizer/events/{id}{rest}");
    let (status, created) = change(json!({ "site": SITE.0 }), event("/create")).await;
    assert_eq!(status, StatusCode::CREATED, "{created}");
    let (status, _) = change(json!({ "sign_ups": true }), event("")).await;
    assert_eq!(status, StatusCode::OK);
    let listed =
        || async { SITE.tournaments(&bridge, &organizer).await.1["tournaments"][0].clone() };
    assert_eq!(listed().await["state"], "draft");
    assert_eq!(listed().await["actions"], json!(["plan", "cancel"]));
    for name in ["plan", "open_sign_ups"] {
        let (status, moved) = change(json!({}), event(&format!("/transitions/{name}"))).await;
        assert_eq!(status, StatusCode::OK, "{name}: {moved}");
    }
    let (status, refused) = change(json!({}), event("/transitions/archive")).await;
    assert_eq!(
        (status, refused["error"]["details"]["reason"].as_str()),
        (StatusCode::BAD_REQUEST, Some("site_refused"))
    );
    assert_eq!(listed().await["state"], "sign_ups");

    // Three players with Discord connected in Ember sign up through Ember.
    let mut players = Vec::new();
    for byte in 1..=3u8 {
        let player = sign_in::player(&bridge, byte).await;
        sites::connect_discord(&bridge, &format!("27422034255875614{byte}"), &player).await;
        let path = format!("/v1/events/{id}/sign-up");
        let (status, signed) = sign_in::send(
            &bridge,
            &player,
            Action::SiteConnect,
            Method::Post,
            &path,
            json!({}),
        )
        .await;
        assert_eq!(status, StatusCode::CREATED, "{signed}");
        players.push(player);
    }
    let tournament = created["tournament"].as_str().unwrap();
    let shown: Json = reqwest::get(format!("{site_origin}/tournaments/{tournament}"))
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    let entrants = shown["entrants"].as_array().unwrap();
    assert_eq!(entrants.len(), 3, "{shown}");
    for (entrant, player) in entrants.iter().zip(&players) {
        assert_eq!(
            entrant["ember_id"],
            player.id().as_str(),
            "linked through the bridge's Discord lookup"
        );
    }

    // Ready and started, first to 2 (the helpers' play takes no other): the
    // connector runs it on the http adapter.
    let starts_at = bridge.clock.now() + 60;
    assert_eq!(
        change(
            json!({ "starts_at": starts_at, "settings": {
                "rounds": { "games_to_win": 2 },
            } }),
            event("")
        )
        .await
        .0,
        StatusCode::OK
    );
    assert_eq!(change(json!({}), event("/ready")).await.0, StatusCode::OK);
    let (status, started) = change(json!({}), event("/start")).await;
    assert_eq!(status, StatusCode::OK, "{started}");
    bridge.clock.advance(61);
    let mut config = support::config(&dir, &bridge.origin, &bridge.provider(CONNECTION).await);
    config.bridge.build_id = play::BUILD.into();
    config.desk = Some(Desk {
        services: [(SITE.0.to_owned(), site_origin.clone())].into(),
        ..Desk::default()
    });
    let connector = support::connector(&config, &Clock::frozen(bridge.clock.now()));
    connector.serve(listener).unwrap();

    // Player 1 has the bye; Player 3 beats Player 2, then Player 1 wins the final.
    let mut generation = 0;
    connector.pass(true).await;
    play(&bridge, &mut players, &[P2Win, P2Win], &mut generation).await;
    ember_bridge::deliver_results(bridge.state()).await;
    connector.pass(true).await;
    connector.pass(true).await;
    play(
        &bridge,
        &mut players,
        &[P1Win, P2Win, P1Win],
        &mut generation,
    )
    .await;
    ember_bridge::deliver_results(bridge.state()).await;
    connector.pass(true).await;
    let finished: Json = reqwest::get(format!("{site_origin}/tournaments/{tournament}"))
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(finished["state"], "finished", "{finished}");
    let last = finished["sets"].as_array().unwrap().last().unwrap().clone();
    assert_eq!(last["winner"], players[0].id().as_str());
    let _ = std::fs::remove_dir_all(dir);
}
