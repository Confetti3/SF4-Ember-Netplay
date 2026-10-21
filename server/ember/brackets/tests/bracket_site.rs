//! The real bracket site (server/bracket-site) played through the `http`
//! adapter: an organizer enters three players by their Discord accounts, which
//! the site looks up on the bridge, and starts the bracket; the connector plays
//! the semifinal and the final in Ember and reports both, and the site moves
//! the winner on and finishes. Needs Node 22.18 or later with TypeScript
//! support on PATH, so it is ignored by default:
//!
//! ```sh
//! cargo test -p ember-brackets --test bracket_site -- --ignored
//! ```
mod support;

use std::{
    net::TcpListener,
    path::Path,
    process::{Child, Command},
    time::Duration,
};

use ember_brackets::Adapter;
use reqwest::Method;
use serde_json::{Value, json};
use support::{Extra, Setup, World};

const CONNECTOR_TOKEN: &str = "bracket-site-connector-token";
const OPERATOR_TOKEN: &str = "bracket-site-operator-token";
const DISCORD: [&str; 3] = [
    "274220342558756141",
    "274220342558756142",
    "274220342558756143",
];

struct Site {
    child: Child,
    origin: String,
    http: reqwest::Client,
}

impl Drop for Site {
    fn drop(&mut self) {
        let _ = self.child.kill();
    }
}

impl Site {
    /// Starts the site on `port`, with the bridge connection's provider
    /// credential for its lookups.
    async fn start(dir: &Path, port: u16, bridge: &str, provider: &str) -> Self {
        let write = |name: &str, text: &str| {
            let path = dir.join(name);
            std::fs::write(&path, text).unwrap();
            path
        };
        let main = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../bracket-site/src/main.ts");
        let child = Command::new("node")
            .arg(main)
            .env("BRACKET_SITE_LISTEN", format!("127.0.0.1:{port}"))
            .env("BRACKET_SITE_DATABASE", dir.join("site.sqlite3"))
            .env(
                "BRACKET_SITE_OPERATOR_TOKEN_FILE",
                write("operator.token", OPERATOR_TOKEN),
            )
            .env(
                "BRACKET_SITE_CONNECTOR_TOKEN_FILE",
                write("connector.token", CONNECTOR_TOKEN),
            )
            .env("EMBER_BRIDGE_ORIGIN", bridge)
            .env(
                "EMBER_BRIDGE_CREDENTIAL_FILE",
                write("provider.token", provider),
            )
            .spawn()
            .expect("node is on PATH");
        let site = Self {
            child,
            origin: format!("http://127.0.0.1:{port}"),
            http: reqwest::Client::new(),
        };
        for _ in 0..100 {
            if site
                .http
                .get(format!("{}/", site.origin))
                .send()
                .await
                .is_ok()
            {
                return site;
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
        panic!("the bracket site did not start");
    }

    /// The operator's request; its status and JSON answer.
    async fn operator(&self, method: Method, path: &str, body: Value) -> (u16, Value) {
        let response = self
            .http
            .request(method, format!("{}{path}", self.origin))
            .bearer_auth(OPERATOR_TOKEN)
            .json(&body)
            .send()
            .await
            .unwrap();
        let status = response.status().as_u16();
        (status, response.json().await.unwrap_or(Value::Null))
    }

    async fn set(&self, id: &str) -> Value {
        let found: Value = self
            .http
            .get(format!("{}/tournaments/weekly", self.origin))
            .send()
            .await
            .unwrap()
            .json()
            .await
            .unwrap();
        found["sets"]
            .as_array()
            .unwrap()
            .iter()
            .find(|set| set["id"] == id)
            .unwrap()
            .clone()
    }
}

/// Plays the bridge's newest match to `winners` and reports it.
async fn play(world: &World, winners: &[u8]) -> String {
    world.connector.pass(true).await;
    let (external, match_id, _) = world.bridge.matches().await.pop().unwrap();
    world.play(&match_id, winners).await;
    world.bridge.deliver().await;
    world.connector.pass(false).await;
    external
}

#[tokio::test]
#[ignore = "needs Node with TypeScript support"]
async fn the_bracket_site_is_played_to_its_final() {
    let port = TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port();
    let world = World::start(Setup {
        challonge: false,
        extra: vec![Extra {
            adapter: Adapter::Http,
            tournament: "weekly".into(),
            api_base: format!("http://127.0.0.1:{port}/tournaments/weekly/ember"),
            token: CONNECTOR_TOKEN,
        }],
        ..Setup::default()
    })
    .await;
    let mut players = Vec::new();
    for (byte, discord) in (1..).zip(DISCORD) {
        players.push(world.bridge.connect_discord(byte, discord).await);
    }
    let dir = support::temp("bracket-site");
    let site = Site::start(&dir, port, &world.bridge.origin, &world.provider).await;

    let (status, _) = site
        .operator(
            Method::POST,
            "/tournaments",
            json!({ "id": "weekly", "title": "Weekly" }),
        )
        .await;
    assert_eq!(status, 201);
    for action in ["plan", "open_sign_ups"] {
        let (status, _) = site
            .operator(
                Method::PUT,
                "/tournaments/weekly/state",
                json!({ "action": action }),
            )
            .await;
        assert_eq!(status, 200);
    }
    for (n, discord) in DISCORD.iter().enumerate() {
        let (status, entrant) = site
            .operator(
                Method::POST,
                "/tournaments/weekly/entrants",
                json!({ "discord": discord, "name": format!("Player {}", n + 1) }),
            )
            .await;
        assert_eq!(status, 201, "{entrant}");
        assert_eq!(entrant["ember_id"], players[n].as_str());
    }
    let (status, started) = site
        .operator(
            Method::PUT,
            "/tournaments/weekly/state",
            json!({ "action": "start" }),
        )
        .await;
    assert_eq!(status, 200, "{started}");

    // Players 2 and 3 play the semifinal, first to 2; Player 3 wins 2-1.
    assert_eq!(play(&world, &[1, 0, 1]).await, "http:weekly:r1-2:1");
    let semifinal = site.set("r1-2").await;
    assert_eq!(semifinal["winner"], players[2].as_str());
    assert_eq!(semifinal["scores"], json!([1, 2]));
    assert_eq!(semifinal["underway"], true);
    let final_ = site.set("r2-1").await;
    assert_eq!(
        final_["entries"],
        json!([players[0].as_str(), players[2].as_str()])
    );

    // The final is first to 3 (support::rounds): Player 1 wins 3-0.
    world.advance(601);
    assert_eq!(play(&world, &[0, 0, 0]).await, "http:weekly:r2-1:1");
    let final_ = site.set("r2-1").await;
    assert_eq!(final_["winner"], players[0].as_str());
    assert_eq!(final_["scores"], json!([3, 0]));
    assert_eq!(world.progress("http:weekly", "r2-1"), "reported");
    let (_, finished) = site
        .operator(Method::GET, "/tournaments/weekly", Value::Null)
        .await;
    assert_eq!(finished["state"], "finished");
    let _ = std::fs::remove_dir_all(dir);
}
