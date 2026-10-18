//! The results URL: only a delivery signed with the connection's result
//! secret, recent, and about this connection is recorded, and each once.
mod support;

use ember_brackets::{
    Bridge, Clock, Config, Connector, Credential, CredentialKind, Results, SiteKind, Tournament,
};
use ember_protocol::{
    partner::result_id,
    webhook::{self, Headers, Secret},
};
use reqwest::StatusCode;
use serde_json::{Value, json};
use support::{CONNECTION, TOURNAMENT, result_secret, rounds, temp};

const KATE: &str = "emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";
const MATCH: &str = "emt_8RGUvPxAAZ0V9oj_D-yBDg";

/// A connector whose bridge and Challonge are never called here.
fn connector(dir: &std::path::Path, clock: Clock) -> Connector {
    std::fs::write(dir.join("provider.token"), "emk_test").unwrap();
    std::fs::write(dir.join("result.secret"), result_secret().reveal().as_str()).unwrap();
    std::fs::write(dir.join("challonge.key"), "key").unwrap();
    let config = Config {
        bridge: Bridge {
            api: "http://127.0.0.1:9".into(),
            credential_file: dir.join("provider.token"),
            connection_id: CONNECTION.into(),
            build_id: "test-build".into(),
        },
        results: Results {
            listen: "127.0.0.1:0".into(),
            path: "/ember/results".into(),
            secret_file: dir.join("result.secret"),
        },
        database: dir.join("brackets.sqlite3"),
        poll_secs: 120,
        max_poll_secs: 600,
        monthly_request_budget: 500,
        tournaments: vec![Tournament {
            site: SiteKind::Challonge,
            tournament: TOURNAMENT.into(),
            credential: Credential {
                kind: CredentialKind::V1Key,
                file: dir.join("challonge.key"),
            },
            rounds: rounds(),
            active_from: None,
            active_until: None,
            api_base: Some("http://127.0.0.1:9/v2.1".into()),
        }],
    };
    Connector::new(config, clock).unwrap()
}

fn result(external: &str) -> Value {
    json!({
        "type": "io.ember.tournament.match.result.v1",
        "bridge_id": "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11",
        "connection_id": CONNECTION,
        "match_id": MATCH,
        "external_match_id": external,
        "outcome": "completed",
        "revision": "6",
        "participants": [
            { "participant_id": "epl_a", "ember_id": KATE, "slot": 0, "score": 2 },
            { "participant_id": "epl_b", "ember_id": KATE, "slot": 1, "score": 0 },
        ],
        "winner_participant_id": "epl_a",
    })
}

fn signed(secret: &Secret, body: &Value, at: u64) -> (Headers, Vec<u8>) {
    let body = serde_json::to_vec(body).unwrap();
    (
        webhook::sign(&[secret], &result_id(MATCH), at, &body).unwrap(),
        body,
    )
}

#[tokio::test]
async fn only_signed_recent_results_for_this_connection_are_recorded_once() {
    let dir = temp("results");
    let clock = Clock::default();
    let connector = connector(&dir, clock.clone());
    let now = clock.now();
    let body = result("challonge:ember_weekly:1001:1");

    let (headers, bytes) = signed(&result_secret(), &body, now);
    assert_eq!(connector.accept(&headers, &bytes), Ok(true));
    assert_eq!(connector.accept(&headers, &bytes), Ok(false));

    // Another secret, an old or future timestamp, or a changed body.
    let (other, bytes) = signed(&Secret::from_bytes([8; 32]), &body, now);
    assert_eq!(
        connector.accept(&other, &bytes),
        Err(StatusCode::UNAUTHORIZED)
    );
    for at in [now - 301, now + 301] {
        let (stale, bytes) = signed(&result_secret(), &body, at);
        assert_eq!(
            connector.accept(&stale, &bytes),
            Err(StatusCode::UNAUTHORIZED)
        );
    }
    let (headers, _) = signed(&result_secret(), &body, now);
    assert_eq!(
        connector.accept(&headers, br#"{"type":"x"}"#),
        Err(StatusCode::UNAUTHORIZED)
    );

    // Signed, but not a result of this connection's match.
    let mut wrong = Vec::new();
    for (pointer, value) in [
        ("connection_id", json!("someone-else")),
        ("type", json!("io.ember.tournament.match.completed.v1")),
        ("winner_participant_id", json!("epl_c")),
        ("outcome", json!("restart")),
    ] {
        let mut changed = body.clone();
        changed[pointer] = value;
        wrong.push(changed);
    }
    let mut swapped = body.clone();
    swapped["participants"][0]["slot"] = json!(1);
    swapped["participants"][1]["slot"] = json!(0);
    wrong.push(swapped);
    for changed in wrong {
        let (headers, bytes) = signed(&result_secret(), &changed, now);
        assert_eq!(
            connector.accept(&headers, &bytes),
            Err(StatusCode::BAD_REQUEST),
            "{changed}"
        );
    }
    let body_bytes = serde_json::to_vec(&body).unwrap();
    let elsewhere = webhook::sign(&[&result_secret()], "res_emt_other", now, &body_bytes).unwrap();
    assert_eq!(
        connector.accept(&elsewhere, &body_bytes),
        Err(StatusCode::BAD_REQUEST)
    );

    // A result for a tournament not configured here is acknowledged.
    let mut unknown = result("challonge:other_cup:1:1");
    unknown["match_id"] = json!("emt_AAAAAAAAAAAAAAAAAAAAAA");
    let unknown_bytes = serde_json::to_vec(&unknown).unwrap();
    let headers = webhook::sign(
        &[&result_secret()],
        "res_emt_AAAAAAAAAAAAAAAAAAAAAA",
        now,
        &unknown_bytes,
    )
    .unwrap();
    assert_eq!(connector.accept(&headers, &unknown_bytes), Ok(true));
    drop(connector);
    std::fs::remove_dir_all(&dir).unwrap();
}

#[tokio::test]
async fn the_results_url_answers_over_http() {
    let dir = temp("results-http");
    let clock = Clock::default();
    let connector = connector(&dir, clock.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let (address, server) = connector.serve(listener).unwrap();
    let url = format!("http://{address}/ember/results");
    let body = result("challonge:ember_weekly:1001:1");
    let send = |headers: Headers, bytes: Vec<u8>| {
        reqwest::Client::new()
            .post(&url)
            .header(webhook::HEADER_ID, headers.id)
            .header(webhook::HEADER_TIMESTAMP, headers.timestamp)
            .header(webhook::HEADER_SIGNATURE, headers.signature)
            .body(bytes)
            .send()
    };
    let (headers, bytes) = signed(&result_secret(), &body, clock.now());
    for _ in 0..2 {
        let answer = send(headers.clone(), bytes.clone()).await.unwrap();
        assert_eq!(answer.status(), StatusCode::NO_CONTENT);
    }
    let (other, bytes) = signed(&Secret::from_bytes([8; 32]), &body, clock.now());
    assert_eq!(
        send(other, bytes).await.unwrap().status(),
        StatusCode::UNAUTHORIZED
    );
    server.abort();
    drop(connector);
    // The aborted server lets go of the database when its task is dropped.
    let _ = std::fs::remove_dir_all(&dir);
}
