//! The results URL: a delivery is recorded once when it is signed with the
//! connection's result secret (or the one it replaces), recent, and a result
//! of this connection. One that fails verification is answered 503, which
//! the bridge retries; a signed one that is not a result is refused for
//! good.
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
const NOW: u64 = 1_792_350_000;

fn previous_secret() -> Secret {
    Secret::from_bytes([6; 32])
}

/// A connector whose bridge and Challonge are never called here.
fn connector(dir: &std::path::Path, clock: Clock) -> Connector {
    std::fs::write(dir.join("provider.token"), "emk_test").unwrap();
    std::fs::write(dir.join("result.secret"), result_secret().reveal().as_str()).unwrap();
    std::fs::write(
        dir.join("previous.secret"),
        previous_secret().reveal().as_str(),
    )
    .unwrap();
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
            previous_secret_file: Some(dir.join("previous.secret")),
        },
        database: dir.join("brackets.sqlite3"),
        poll_secs: 120,
        max_poll_secs: 600,
        monthly_request_budget: 500,
        allow_loopback_http: true,
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
            mark_underway: true,
            api_base: Some("http://127.0.0.1:9".into()),
        }],
    };
    Connector::new(config, clock).unwrap()
}

fn result(external: &str, match_id: &str) -> Value {
    json!({
        "type": "io.ember.tournament.match.result.v1",
        "bridge_id": "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11",
        "connection_id": CONNECTION,
        "match_id": match_id,
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
    let id = result_id(body["match_id"].as_str().unwrap());
    let body = serde_json::to_vec(body).unwrap();
    (webhook::sign(&[secret], &id, at, &body).unwrap(), body)
}

#[tokio::test]
async fn only_signed_recent_results_for_this_connection_are_recorded_once() {
    let dir = temp("results");
    let connector = connector(&dir, Clock::frozen(NOW));
    let body = result("challonge:ember_weekly:1001:1", MATCH);
    let fresh = |n: u8| {
        let mut body = result("challonge:ember_weekly:1001:1", &format!("emt_{n:0>22}"));
        body["external_match_id"] = json!(format!("challonge:ember_weekly:{n}:1"));
        body
    };

    let (headers, bytes) = signed(&result_secret(), &body, NOW);
    assert_eq!(connector.accept(&headers, &bytes), Ok(true));
    assert_eq!(connector.accept(&headers, &bytes), Ok(false));

    // Five minutes either way is recent; a second more is not, and is
    // answered so that the bridge tries again.
    for (n, at) in [(1, NOW - 300), (2, NOW + 300)] {
        let (headers, bytes) = signed(&result_secret(), &fresh(n), at);
        assert_eq!(connector.accept(&headers, &bytes), Ok(true), "{at}");
    }
    for at in [NOW - 301, NOW + 301] {
        let (stale, bytes) = signed(&result_secret(), &fresh(3), at);
        assert_eq!(
            connector.accept(&stale, &bytes),
            Err(StatusCode::SERVICE_UNAVAILABLE)
        );
    }
    // The secret it replaces still works; another, or a changed body, does not.
    let (old, bytes) = signed(&previous_secret(), &fresh(4), NOW);
    assert_eq!(connector.accept(&old, &bytes), Ok(true));
    let (other, bytes) = signed(&Secret::from_bytes([8; 32]), &fresh(5), NOW);
    assert_eq!(
        connector.accept(&other, &bytes),
        Err(StatusCode::SERVICE_UNAVAILABLE)
    );
    let (headers, _) = signed(&result_secret(), &fresh(5), NOW);
    assert_eq!(
        connector.accept(&headers, br#"{"type":"x"}"#),
        Err(StatusCode::SERVICE_UNAVAILABLE)
    );

    // Signed, but not a result of this connection's match: refused for good.
    let mut wrong = Vec::new();
    for (field, value) in [
        ("connection_id", json!("someone-else")),
        ("type", json!("io.ember.tournament.match.completed.v1")),
        ("winner_participant_id", json!("epl_c")),
        ("outcome", json!("restart")),
        ("external_match_id", json!("not-ours")),
    ] {
        let mut changed = fresh(6);
        changed[field] = value;
        wrong.push(changed);
    }
    let mut slots = fresh(6);
    slots["participants"][1]["slot"] = json!(0);
    wrong.push(slots);
    for changed in wrong {
        let (headers, bytes) = signed(&result_secret(), &changed, NOW);
        assert_eq!(
            connector.accept(&headers, &bytes),
            Err(StatusCode::BAD_REQUEST),
            "{changed}"
        );
    }
    let body_bytes = serde_json::to_vec(&fresh(7)).unwrap();
    let elsewhere = webhook::sign(&[&result_secret()], "res_emt_other", NOW, &body_bytes).unwrap();
    assert_eq!(
        connector.accept(&elsewhere, &body_bytes),
        Err(StatusCode::BAD_REQUEST)
    );

    // Participants listed slot 1 first are read by slot.
    let mut reordered = fresh(8);
    let participants = reordered["participants"].as_array_mut().unwrap();
    participants.reverse();
    let (headers, bytes) = signed(&result_secret(), &reordered, NOW);
    assert_eq!(connector.accept(&headers, &bytes), Ok(true));

    // A result for a tournament not configured here is acknowledged.
    let mut unknown = fresh(9);
    unknown["external_match_id"] = json!("challonge:other_cup:1:1");
    let (headers, bytes) = signed(&result_secret(), &unknown, NOW);
    assert_eq!(connector.accept(&headers, &bytes), Ok(true));
    drop(connector);
    std::fs::remove_dir_all(&dir).unwrap();
}

#[tokio::test]
async fn the_results_url_answers_over_http() {
    let dir = temp("results-http");
    let connector = connector(&dir, Clock::frozen(NOW));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let (address, server) = connector.serve(listener).unwrap();
    let url = format!("http://{address}/ember/results");
    let client = reqwest::Client::new();
    let send = |headers: Option<Headers>, bytes: Vec<u8>| {
        let mut request = client.post(&url).body(bytes);
        if let Some(headers) = headers {
            request = request
                .header(webhook::HEADER_ID, headers.id)
                .header(webhook::HEADER_TIMESTAMP, headers.timestamp)
                .header(webhook::HEADER_SIGNATURE, headers.signature);
        }
        request.send()
    };
    let body = result("challonge:ember_weekly:1001:1", MATCH);
    let (headers, bytes) = signed(&result_secret(), &body, NOW);
    for _ in 0..2 {
        let answer = send(Some(headers.clone()), bytes.clone()).await.unwrap();
        assert_eq!(answer.status(), StatusCode::NO_CONTENT);
    }
    let (other, bytes) = signed(&Secret::from_bytes([8; 32]), &body, NOW);
    assert_eq!(
        send(Some(other), bytes.clone()).await.unwrap().status(),
        StatusCode::SERVICE_UNAVAILABLE
    );
    assert_eq!(
        send(None, bytes).await.unwrap().status(),
        StatusCode::SERVICE_UNAVAILABLE
    );
    let huge = vec![b' '; 65 * 1024];
    assert_eq!(
        send(Some(headers), huge).await.unwrap().status(),
        StatusCode::PAYLOAD_TOO_LARGE
    );
    server.abort();
    drop(connector);
    // The aborted server lets go of the database when its task is dropped.
    let _ = std::fs::remove_dir_all(&dir);
}
