//! The public replay archive (docs/design/REPLAY_ARCHIVE.md): any game a
//! player sends, and every tournament set's game, kept once, stored in the
//! bucket with the set's pack, then listed for the archive page.
mod common;

use std::sync::{Arc, Mutex};

use common::{
    Bridge, Player,
    play::{Table, create_match, played, replay, send_replay},
};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use sha2::{Digest, Sha256};

/// What the stand-in bucket was sent: each PUT's path and body.
type Stored = Arc<Mutex<Vec<(String, Vec<u8>)>>>;

/// An S3 stand-in that takes any signed PUT whose content hash is right.
async fn bucket() -> (String, Stored) {
    let stored: Stored = Arc::default();
    let keep = stored.clone();
    let app = axum::Router::new().fallback(
        move |method: axum::http::Method,
              uri: axum::http::Uri,
              headers: axum::http::HeaderMap,
              body: axum::body::Bytes| {
            let keep = keep.clone();
            async move {
                let hash: String = Sha256::digest(&body)
                    .iter()
                    .map(|b| format!("{b:02x}"))
                    .collect();
                let signed = headers
                    .get("authorization")
                    .and_then(|v| v.to_str().ok())
                    .is_some_and(|v| v.starts_with("AWS4-HMAC-SHA256 Credential=ember-test/"));
                if method != axum::http::Method::PUT
                    || !signed
                    || headers
                        .get("x-amz-content-sha256")
                        .and_then(|v| v.to_str().ok())
                        != Some(hash.as_str())
                {
                    return StatusCode::FORBIDDEN;
                }
                keep.lock()
                    .unwrap()
                    .push((uri.path().to_owned(), body.to_vec()));
                StatusCode::OK
            }
        },
    );
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    (origin, stored)
}

struct Fixture {
    bridge: Bridge,
    provider: String,
    a: Player,
    b: Player,
    links: [Json; 2],
    stored: Stored,
}

impl Table for Fixture {
    fn bridge(&self) -> &Bridge {
        &self.bridge
    }
    fn a(&self) -> &Player {
        &self.a
    }
    fn b(&self) -> &Player {
        &self.b
    }
}

async fn fixture() -> Fixture {
    let (endpoint, stored) = bucket().await;
    let secrets = ember_bridge::integrations::Secrets {
        archive_secret_key: Some(zeroize::Zeroizing::new("secret".into())),
        ..Default::default()
    };
    let bridge = Bridge::start_with(
        move |config| {
            config.integration_secrets = Some(std::env::temp_dir().join("ember-archive-none.json"));
            config.archive = Some(ember_bridge::config::Archive {
                endpoint,
                bucket: "replays".into(),
                region: "us-east-1".into(),
                access_key: "ember-test".into(),
                public_base: "https://s3.example/replays".into(),
            });
        },
        secrets,
    )
    .await;
    let provider = bridge.provider("mock-a").await;
    let mut a = bridge.player(41);
    let mut b = bridge.player(42);
    bridge.open_session(&mut a).await;
    bridge.open_session(&mut b).await;
    let link_a = bridge.link(&provider, &a, "mock-a", "fighter-a").await;
    let link_b = bridge.link(&provider, &b, "mock-a", "fighter-b").await;
    Fixture {
        bridge,
        provider,
        a,
        b,
        links: [link_a, link_b],
        stored,
    }
}

/// Game `n` with fighters `fighters` in its record (native IDs).
fn game(n: u8, fighters: [u8; 2]) -> Vec<u8> {
    let mut replay = replay(n);
    for (side, fighter) in fighters.into_iter().enumerate() {
        replay[8 + 47 + 8 + side * 34 + 16] = fighter;
    }
    replay
}

async fn send(f: &Fixture, sender: &Player, replay: &[u8], query: &str) -> StatusCode {
    let crc = ember_protocol::replays::crc_name(replay).unwrap();
    f.bridge
        .client
        .put(f.bridge.url(&format!("/v1/replays/{crc}?{query}")))
        .bearer_auth(sender.token())
        .body(replay.to_vec())
        .send()
        .await
        .unwrap()
        .status()
}

async fn listed(f: &Fixture, query: &str) -> Json {
    let response = f
        .bridge
        .client
        .get(f.bridge.url(&format!("/v1/archive?{query}")))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    serde_json::from_slice(&response.bytes().await.unwrap()).unwrap()
}

/// Waits until the bucket holds `count` files.
async fn stored(f: &Fixture, count: usize) -> Vec<String> {
    for _ in 0..100 {
        let paths: Vec<String> = f
            .stored
            .lock()
            .unwrap()
            .iter()
            .map(|(path, _)| path.clone())
            .collect();
        if paths.len() >= count {
            return paths;
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    panic!("the bucket holds {:?}", f.stored.lock().unwrap().len());
}

#[tokio::test]
async fn any_game_is_kept_once_with_both_sides_countries_then_stored_and_listed() {
    let f = fixture().await;
    let played = game(7, [20, 34]);
    assert_eq!(
        send(&f, &f.a, &played, "a=Ann&b=Bob&side=0&country=BE").await,
        StatusCode::CREATED
    );
    // The opponent's copy of the same game adds their country only.
    assert_eq!(
        send(&f, &f.b, &played, "a=Eve&b=Bob&side=1&country=NL").await,
        StatusCode::OK
    );
    let mut torn = game(8, [0, 0]);
    torn.pop();
    assert_eq!(send(&f, &f.a, &torn, "").await, StatusCode::BAD_REQUEST);
    let anonymous = f
        .bridge
        .client
        .put(f.bridge.url("/v1/replays/00000000"))
        .body(played.clone())
        .send()
        .await
        .unwrap();
    assert_eq!(anonymous.status(), StatusCode::UNAUTHORIZED);
    let crc = ember_protocol::replays::crc_name(&played).unwrap();
    assert_eq!(
        stored(&f, 1).await,
        vec![format!("/replays/games/{crc}.emberreplay")]
    );
    assert_eq!(f.stored.lock().unwrap()[0].1, played);
    // Stored, the replay leaves the bridge.
    let mut list = Json::Null;
    for _ in 0..100 {
        list = listed(&f, "").await;
        if !list["games"].as_array().unwrap().is_empty() {
            break;
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    let listed_game = &list["games"][0];
    assert_eq!(listed_game["names"], json!(["Ann", "Bob"]));
    assert_eq!(listed_game["countries"], json!(["BE", "NL"]));
    assert_eq!(listed_game["fighters"], json!([20, 34]));
    assert_eq!(
        listed_game["url"],
        format!("https://s3.example/replays/games/{crc}.emberreplay")
    );
    assert_eq!(list["countries"], json!(["BE", "NL"]));
    let crc_held = crc.clone();
    let held: Option<Vec<u8>> = f
        .bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT replay FROM archive_games WHERE crc = ?1",
                [crc_held],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap();
    assert!(held.is_none());
    for (query, count) in [
        ("fighter=34", 1),
        ("fighter=1", 0),
        ("country=NL", 1),
        ("country=JP", 0),
        ("q=an", 1),
        ("event=online", 1),
        ("round=top8", 0),
        ("before=1", 0),
    ] {
        assert_eq!(
            listed(&f, query).await["games"].as_array().unwrap().len(),
            count,
            "{query}"
        );
    }
}

#[tokio::test]
async fn a_sets_games_are_archived_with_its_pack() {
    let f = fixture().await;
    let id = create_match(
        &f.bridge,
        &f.provider,
        &f.links,
        [&f.a, &f.b],
        "archive-set",
        "ember-room-v1",
        2,
    )
    .await;
    played(&f, &id, 2).await;
    for n in [1, 2] {
        assert_eq!(
            send_replay(&f, &f.a, &id, &replay(n), 1).await,
            StatusCode::CREATED
        );
    }
    let paths = stored(&f, 4).await;
    assert!(
        paths.contains(&format!("/replays/sets/{id}.emberpack")),
        "{paths:?}"
    );
    let list = listed(&f, "").await;
    assert_eq!(list["games"].as_array().unwrap().len(), 2);
    assert_eq!(list["games"][0]["set"], id.as_str());
    assert_eq!(
        list["sets"][&id]["pack"],
        format!("https://s3.example/replays/sets/{id}.emberpack")
    );
    assert_eq!(
        listed(&f, "event=online").await["games"]
            .as_array()
            .unwrap()
            .len(),
        0
    );
}

#[tokio::test]
async fn without_an_archive_nothing_is_taken() {
    let bridge = Bridge::start().await;
    let mut a = bridge.player(43);
    bridge.open_session(&mut a).await;
    let played = replay(1);
    let crc = ember_protocol::replays::crc_name(&played).unwrap();
    let response = bridge
        .client
        .put(bridge.url(&format!("/v1/replays/{crc}")))
        .bearer_auth(a.token())
        .body(played)
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::NOT_FOUND);
    let listed = bridge
        .client
        .get(bridge.url("/v1/archive"))
        .send()
        .await
        .unwrap();
    assert_eq!(listed.status(), StatusCode::NOT_FOUND);
}
