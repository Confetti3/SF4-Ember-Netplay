//! BluMint's partner API against a stand-in BluMint: player lookup by Discord
//! account, match creation with a Play link, status, result submission with
//! retries and restarts, and registration of the three endpoints.
mod common;

use std::sync::{Arc, Mutex};

use axum::{
    Router,
    body::Bytes,
    extract::State,
    http::{HeaderMap, StatusCode as AxumStatus, Uri},
    routing::post,
};
use common::{Bridge, Player};
use ember_bridge::{config, integrations::Secrets};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use zeroize::Zeroizing;

const KATE: &str = "274220342558756145";
const SAM: &str = "1725265127372152396";
const CONNECTION: &str = "blumint-test";

/// What the stand-in BluMint was sent, and how many calls to refuse with 500 first.
#[derive(Default)]
struct Received {
    calls: Vec<(String, String, Json)>,
    fail: usize,
}

type Shared = Arc<Mutex<Received>>;

async fn fake_blumint() -> (String, Shared) {
    async fn record(
        State(shared): State<Shared>,
        uri: Uri,
        headers: HeaderMap,
        body: Bytes,
    ) -> AxumStatus {
        let mut received = shared.lock().unwrap();
        if received.fail > 0 {
            received.fail -= 1;
            return AxumStatus::INTERNAL_SERVER_ERROR;
        }
        let key = headers
            .get("x-api-key")
            .and_then(|v| v.to_str().ok())
            .unwrap_or("")
            .to_owned();
        received.calls.push((
            uri.path().to_owned(),
            key,
            serde_json::from_slice(&body).unwrap(),
        ));
        AxumStatus::OK
    }
    let shared = Shared::default();
    let app = Router::new()
        .route("/api/tournaments/match/submit", post(record))
        .route("/api/tournaments/setLookupPlayer", post(record))
        .route("/api/tournaments/match/setCreate", post(record))
        .route("/api/tournaments/match/setRetrieveStatus", post(record))
        .with_state(shared.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let base = format!("http://{}/api", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await });
    (base, shared)
}

struct Fixture {
    bridge: Bridge,
    blumint: Shared,
    provider: String,
    organizer: String,
    kate: Player,
    sam: Player,
}

async fn fixture() -> Fixture {
    let (api_base, blumint) = fake_blumint().await;
    let bridge = Bridge::start_with(
        |config| {
            config.tenants.push(config::Tenant {
                id: "bm".into(),
                name: "BluMint".into(),
                connections: vec![config::Connection {
                    id: CONNECTION.into(),
                    kind: "blumint".into(),
                    environment: "staging".into(),
                    display_name: "BluMint (test)".into(),
                    enabled: true,
                    api_base: Some(api_base),
                }],
            });
        },
        Secrets {
            api_keys: [(CONNECTION.to_owned(), Zeroizing::new("bm-key".to_owned()))].into(),
            ..Default::default()
        },
    )
    .await;
    let provider = bridge.provider(CONNECTION).await;
    let organizer = bridge.organizer("bm").await;
    let (mut kate, mut sam) = (bridge.player(1), bridge.player(2));
    bridge.open_session(&mut kate).await;
    bridge.open_session(&mut sam).await;
    // Both connected Discord (tests/discord.rs covers the sign-in itself).
    for (user_id, name, player) in [(KATE, "kate", &kate), (SAM, "sam", &sam)] {
        let ember_id = player.id().as_str().to_owned();
        bridge
            .state()
            .db
            .write(move |tx| {
                tx.execute(
                    "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, ?3, 1)",
                    rusqlite::params![user_id, ember_id, name],
                )?;
                Ok(())
            })
            .await
            .unwrap();
    }
    Fixture {
        bridge,
        blumint,
        provider,
        organizer,
        kate,
        sam,
    }
}

async fn lookup(f: &Fixture, body: Json) -> Json {
    let (status, found) = f.bridge.post(&f.provider, "/v1/blumint/lookup", body).await;
    assert_eq!(status, StatusCode::OK, "{found}");
    found
}

async fn create(f: &Fixture, settings: Json) -> (StatusCode, Json) {
    f.bridge
        .post(
            &f.provider,
            "/v1/blumint/matches",
            json!({
                "teams": [ { "players": [ { "inGameId": f.kate.id() } ] }, { "players": [ { "playerId": f.sam.id() } ] } ],
                "matchSettings": settings,
            }),
        )
        .await
}

async fn match_status(f: &Fixture, id: &str) -> Json {
    let (status, body) = f
        .bridge
        .get(
            &f.provider,
            &format!("/v1/blumint/matches/status?matchId={id}"),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{body}");
    body
}

fn submitted(f: &Fixture) -> Vec<Json> {
    let received = f.blumint.lock().unwrap();
    received
        .calls
        .iter()
        .filter(|(path, key, _)| path == "/api/tournaments/match/submit" && key == "bm-key")
        .map(|(_, _, body)| body.clone())
        .collect()
}

async fn win(f: &Fixture, id: &str, slot: u8, key: &str) {
    let (status, decided) = f
        .bridge
        .post_keyed(
            &f.organizer,
            &format!("/v1/matches/{id}/adjudications"),
            json!({ "kind": "game_result", "winner_slot": slot, "reason": "test", "expected_revision": status_revision(f, id).await }),
            Some(key),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{decided}");
}

async fn status_revision(f: &Fixture, id: &str) -> String {
    let (_, found) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    found["revision"].as_str().unwrap().to_owned()
}

#[tokio::test]
async fn lookup_finds_players_by_their_discord_accounts() {
    let f = fixture().await;
    // Every sign-in method arrives; only Discord is answered, emails skipped.
    let found = lookup(
        &f,
        json!({ "email": ["kate@example.com"], "steam": ["76561198833313974"], "discord": [KATE, "kate@example.com"] }),
    )
    .await;
    assert_eq!(found, json!({ "discord": [f.kate.id()] }));
    assert_eq!(
        lookup(&f, json!({ "discord": ["99999999999999999"] })).await,
        json!({ "discord": [] })
    );
    assert_eq!(
        lookup(&f, json!({ "email": ["sam@example.com"] })).await,
        json!({ "discord": [] })
    );
    // Only a blumint connection's credential reaches these endpoints.
    let other = f.bridge.provider("mock-a").await;
    let (status, _) = f
        .bridge
        .post(&other, "/v1/blumint/lookup", json!({ "discord": [KATE] }))
        .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
}

#[tokio::test]
async fn a_match_is_created_played_and_its_score_sent_to_blumint() {
    let f = fixture().await;
    // A player BluMint never looked up cannot be put in a match.
    let (status, _) = create(&f, json!({})).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (status, _) = f
        .bridge
        .post(
            &f.provider,
            "/v1/blumint/matches",
            json!({ "teams": [ { "players": [ { "inGameId": f.kate.id() } ] } ] }),
        )
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = create(&f, json!({ "gamesToWin": 4 })).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);

    let (status, created) = create(&f, json!({ "gamesToWin": 1, "gravity": 1.1 })).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    let id = created["matchId"].as_str().unwrap().to_owned();
    assert_eq!(
        created["matchUrl"],
        format!("https://embernetplay.link/m#{}/{id}", f.bridge.bridge_id).as_str()
    );
    let before = match_status(&f, &id).await;
    assert_eq!(before["status"], "pending");
    assert_eq!(
        before["teams"][0]["players"][0],
        json!({ "inGameId": f.kate.id(), "status": "absent" })
    );
    assert_eq!(before["teams"][1]["score"], 0);

    // Nothing is sent before the match ends.
    ember_bridge::deliver_to_blumint(f.bridge.state()).await;
    assert!(submitted(&f).is_empty());
    win(&f, &id, 1, "game-1").await;
    assert_eq!(match_status(&f, &id).await["status"], "complete");
    // BluMint is down at first: the result is sent again later, once.
    f.blumint.lock().unwrap().fail = 1;
    ember_bridge::deliver_to_blumint(f.bridge.state()).await;
    assert!(submitted(&f).is_empty());
    f.bridge.clock.advance(11);
    ember_bridge::deliver_to_blumint(f.bridge.state()).await;
    ember_bridge::deliver_to_blumint(f.bridge.state()).await;
    assert_eq!(
        submitted(&f),
        vec![json!({ "matchId": id, "teams": [
            { "score": 0, "players": [ { "inGameId": f.kate.id() } ] },
            { "score": 1, "players": [ { "inGameId": f.sam.id() } ] },
        ] })]
    );
}

#[tokio::test]
async fn cancelled_and_disputed_matches_are_restarted_on_blumint() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, json!({})).await;
    let first = created["matchId"].as_str().unwrap().to_owned();
    let (status, _) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/matches/{first}/cancel"),
            json!({ "reason": "No show", "expected_revision": status_revision(&f, &first).await }),
            Some("cancel-1"),
        )
        .await;
    assert_eq!(status, StatusCode::OK);
    ember_bridge::deliver_to_blumint(f.bridge.state()).await;
    assert_eq!(
        submitted(&f),
        vec![json!({ "matchId": first, "mustRestart": true })]
    );

    // A result the two games disputed has no organizer at BluMint: the match
    // is cancelled here, its players are free, and BluMint restarts it.
    let (_, created) = create(&f, json!({})).await;
    let second = created["matchId"].as_str().unwrap().to_owned();
    let id = second.clone();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE matches SET state = 'needs_review' WHERE id = ?1",
                [&id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    assert_eq!(match_status(&f, &second).await["status"], "running");
    ember_bridge::deliver_to_blumint(f.bridge.state()).await;
    assert_eq!(match_status(&f, &second).await["status"], "cancelled");
    assert_eq!(
        submitted(&f).last().unwrap(),
        &json!({ "matchId": second, "mustRestart": true })
    );
    let (status, _) = create(&f, json!({})).await;
    assert_eq!(
        status,
        StatusCode::OK,
        "the restarted match could not be created"
    );
}

#[tokio::test]
async fn registration_tells_blumint_where_to_call_with_a_working_credential() {
    let f = fixture().await;
    ember_bridge::register_blumint(f.bridge.state(), CONNECTION)
        .await
        .unwrap();
    let calls = f.blumint.lock().unwrap().calls.clone();
    let urls: Vec<_> = calls
        .iter()
        .map(|(path, key, body)| {
            assert_eq!(key, "bm-key");
            let field = [
                "lookupPlayerUrl",
                "createMatchUrl",
                "retrieveMatchStatusUrl",
            ]
            .into_iter()
            .find(|field| body.get(field).is_some())
            .unwrap();
            (
                path.clone(),
                body[field].as_str().unwrap().to_owned(),
                body["authentication"].clone(),
            )
        })
        .collect();
    assert_eq!(urls[0].1, f.bridge.url("/v1/blumint/lookup"));
    assert_eq!(urls[1].1, f.bridge.url("/v1/blumint/matches"));
    assert_eq!(urls[2].1, f.bridge.url("/v1/blumint/matches/status"));
    let authentication = &urls[0].2;
    assert_eq!(
        authentication["config"],
        json!({ "location": "header", "key": "Authorization", "valuePrefix": "Bearer " })
    );
    // BluMint calls back with the credential it was handed.
    let token = authentication["apiKey"].as_str().unwrap();
    let (status, _) = f
        .bridge
        .post(token, "/v1/blumint/lookup", json!({ "discord": [KATE] }))
        .await;
    assert_eq!(status, StatusCode::OK);
    // A connection that is not BluMint's is refused.
    assert!(
        ember_bridge::register_blumint(f.bridge.state(), "mock-a")
            .await
            .is_err()
    );
}
