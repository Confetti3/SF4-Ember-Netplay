//! Read-only checks against the real Challonge and start.gg, with an
//! organizer's tokens from the environment (README.md here says which).
//! Ignored by default. They send what the bridge sends for a signed-in
//! organizer (the user and their tournaments, from the shipped site
//! definitions), and one connector pass over a tournament that changes
//! nothing on the site: no set is marked underway, and with no player
//! linked on the local bridge no match is made and nothing is reported.
mod support;

use ember_brackets::{Adapter, Credential, CredentialKind, Hold, Tournament};
use serde_json::Value;
use support::{bridge::TestBridge, connector, rounds, temp};

/// An environment variable that is set and not empty.
fn env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|value| !value.is_empty())
}

/// The shipped site definition `name`.
fn definition(name: &str) -> Value {
    let path = format!(
        "{}/../bridge/deploy/sites/{name}.json",
        env!("CARGO_MANIFEST_DIR")
    );
    serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap()
}

/// Sends a definition's request with `token`, as the bridge does; the
/// answer.
async fn send(request: &Value, token: &str) -> Value {
    let client = reqwest::Client::new();
    let url = request["url"].as_str().unwrap();
    let mut builder = match request["method"].as_str() {
        Some("POST") => client.post(url),
        _ => client.get(url),
    }
    .bearer_auth(token);
    let headers = request["headers"].as_object();
    for (name, value) in headers.into_iter().flatten() {
        builder = builder.header(name, value.as_str().unwrap());
    }
    if let Some(body) = request.get("body") {
        if !headers.is_some_and(|headers| headers.contains_key("Content-Type")) {
            builder = builder.header("content-type", "application/json");
        }
        builder = builder.body(body.to_string());
    }
    let answer = builder.send().await.unwrap();
    let status = answer.status();
    let body: Value = answer.json().await.unwrap_or(Value::Null);
    assert!(status.is_success(), "{url}: {status} {body}");
    body
}

/// The site's user and organizer tournament list for `token`, read as the
/// definition says.
async fn organizer(site: &str, token: &str) {
    let _ = rustls::crypto::ring::default_provider().install_default();
    let definition = definition(site);
    let user = send(&definition["user"], token).await;
    let id = user.pointer(definition["user"]["id_pointer"].as_str().unwrap());
    assert!(id.is_some_and(|id| !id.is_null()), "{site} user: {user}");
    let read = &definition["organizer"]["tournaments"];
    let listed = send(read, token).await;
    let list = listed.pointer(read["list_pointer"].as_str().unwrap());
    assert!(
        list.is_some_and(Value::is_array),
        "{site} tournaments: {listed}"
    );
    eprintln!(
        "{site}: user {id:?}, {} tournaments",
        list.unwrap().as_array().unwrap().len()
    );
}

/// One connector pass over the site's `tournament`, against a local bridge
/// that links nobody.
async fn read_bracket(adapter: Adapter, tournament: String, token: &str) {
    let dir = temp("live");
    let bridge = TestBridge::start("http://127.0.0.1:9/results", false).await;
    let mut config = support::config(&dir, &bridge.origin, &bridge.provider().await);
    let file = dir.join("site.token");
    std::fs::write(&file, token).unwrap();
    config.tournaments.push(Tournament {
        adapter,
        bridge_site: None,
        tournament,
        credential: Credential {
            kind: CredentialKind::Bearer,
            file,
        },
        rounds: rounds(),
        hold: Hold::default(),
        active_from: None,
        active_until: None,
        mark_underway: false,
        api_base: None,
    });
    let connector = connector(&config, &ember_brackets::Clock::default());
    connector.pass(true).await;
    let database = rusqlite::Connection::open(&config.database).unwrap();
    let asked: u32 = database
        .query_row("SELECT COALESCE(SUM(count), 0) FROM requests", [], |row| {
            row.get(0)
        })
        .unwrap();
    assert!(asked > 0, "the connector asked the site nothing");
    assert!(bridge.matches().await.is_empty());
    let _ = std::fs::remove_dir_all(&dir);
}

#[tokio::test]
#[ignore = "calls the real Challonge; see tests/README.md"]
async fn challonge_answers_as_the_bridge_and_connector_read_it() {
    let Some(token) = env("EMBER_LIVE_CHALLONGE_TOKEN") else {
        eprintln!("EMBER_LIVE_CHALLONGE_TOKEN is not set: skipped");
        return;
    };
    organizer("challonge", &token).await;
    if let Some(tournament) = env("EMBER_LIVE_CHALLONGE_TOURNAMENT") {
        read_bracket(Adapter::Challonge, tournament, &token).await;
    }
}

#[tokio::test]
#[ignore = "calls the real start.gg; see tests/README.md"]
async fn startgg_answers_as_the_bridge_and_connector_read_it() {
    let Some(token) = env("EMBER_LIVE_STARTGG_TOKEN") else {
        eprintln!("EMBER_LIVE_STARTGG_TOKEN is not set: skipped");
        return;
    };
    organizer("startgg", &token).await;
    if let Some(event) = env("EMBER_LIVE_STARTGG_EVENT") {
        read_bracket(Adapter::Startgg, event, &token).await;
    }
}
