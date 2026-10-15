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
use common::{Bridge, Player, results};
use ember_bridge::{AppState, Keys, config, integrations::Secrets};
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

fn keyed() -> Secrets {
    Secrets {
        api_keys: [(CONNECTION.to_owned(), Zeroizing::new("bm-key".to_owned()))].into(),
        ..Default::default()
    }
}

async fn fixture() -> Fixture {
    fixture_with(keyed()).await
}

/// The fixture with these integration secrets.
async fn fixture_with(secrets: Secrets) -> Fixture {
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
                    discord_lookup: false,
                    disputes: None,
                    results_url: None,
                    rooms: None,
                }],
            });
        },
        secrets,
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
    results::win(&f.bridge, &f.provider, &f.organizer, id, slot, key).await;
}

async fn void_last(f: &Fixture, id: &str, key: &str) -> StatusCode {
    results::void_last(&f.bridge, &f.provider, &f.organizer, id, key)
        .await
        .0
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
    // BluMint's connection makes matches only through these endpoints, so
    // every match on it is one BluMint knows.
    for path in ["/v1/matches", "/v1/lobbies", "/v1/tournaments"] {
        let (status, refused) = f
            .bridge
            .post_keyed(&f.provider, path, json!({}), Some("generic"))
            .await;
        assert_eq!(status, StatusCode::FORBIDDEN, "{path}: {refused}");
    }
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
    for refused in [0, 11, 300] {
        let (status, _) = create(&f, json!({ "gamesToWin": refused })).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{refused}");
    }
    // One player on both sides is refused, and nothing is created.
    let (status, refused) = f
        .bridge
        .post(
            &f.provider,
            "/v1/blumint/matches",
            json!({ "teams": [ { "players": [ { "inGameId": f.kate.id() } ] }, { "players": [ { "inGameId": f.kate.id() } ] } ] }),
        )
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{refused}");
    let matches: i64 = f
        .bridge
        .state()
        .db
        .read(|tx| Ok(tx.query_row("SELECT COUNT(*) FROM matches", [], |row| row.get(0))?))
        .await
        .unwrap();
    assert_eq!(matches, 0);

    let (status, created) = create(&f, json!({ "gamesToWin": 1, "gravity": 1.1 })).await;
    assert_eq!(status, StatusCode::OK, "{created}");
    let id = created["matchId"].as_str().unwrap().to_owned();
    // BluMint lost the answer and asks again: the same match. A different
    // request for the same players is refused while it is on. A first to 4
    // (best of 7) passes the length check and meets the match that is on.
    assert_eq!(
        create(&f, json!({ "gamesToWin": 1, "gravity": 1.1 })).await,
        (StatusCode::OK, created.clone())
    );
    assert_eq!(
        create(&f, json!({ "gamesToWin": 4 })).await.0,
        StatusCode::CONFLICT
    );
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
    ember_bridge::deliver_results(f.bridge.state()).await;
    assert!(submitted(&f).is_empty());
    // BluMint is down at first: the result is sent again later, once. The
    // bridge's own worker runs alongside these passes; either may send.
    f.blumint.lock().unwrap().fail = 1;
    win(&f, &id, 1, "game-1").await;
    assert_eq!(match_status(&f, &id).await["status"], "complete");
    results::until(&f.bridge, &id, |(state, attempts)| {
        state == "retrying" && attempts == 1
    })
    .await;
    assert!(submitted(&f).is_empty());
    f.bridge.clock.advance(11);
    results::until(&f.bridge, &id, |(state, _)| state == "delivered").await;
    ember_bridge::deliver_results(f.bridge.state()).await;
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("delivered".into(), 2)
    );
    assert_eq!(
        submitted(&f),
        vec![json!({ "matchId": id, "teams": [
            { "score": 0, "players": [ { "inGameId": f.kate.id() } ] },
            { "score": 1, "players": [ { "inGameId": f.sam.id() } ] },
        ] })]
    );
}

#[tokio::test]
async fn a_completed_match_says_its_result_is_to_be_sent() {
    // Without a key nothing is sent, so the state stays as completion left it.
    let f = fixture_with(Secrets::default()).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, json!({ "gamesToWin": 1 })).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 0, "game-1").await;
    let completed: Vec<Json> = f
        .bridge
        .events(&f.provider, "0")
        .await
        .into_iter()
        .filter(|event| event["type"] == "io.ember.tournament.match.completed.v1")
        .collect();
    assert_eq!(completed.len(), 1);
    assert_eq!(completed[0]["data"]["provider_delivery_state"], "queued");
    let (_, snapshot) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(snapshot["provider_delivery_state"], "queued");
}

#[tokio::test]
async fn a_result_is_final_once_it_is_being_sent() {
    // Without a key nothing is sent yet: a correction still reopens the match.
    let f = fixture_with(Secrets::default()).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, json!({ "gamesToWin": 1 })).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    win(&f, &id, 0, "game-1").await;
    assert_eq!(void_last(&f, &id, "void-1").await, StatusCode::CREATED);
    assert_eq!(match_status(&f, &id).await["status"], "running");
    win(&f, &id, 1, "game-2").await;

    // Once the bridge starts sending the result, it stands, sent or not.
    let state = f.bridge.state();
    let keyed = AppState::new(
        (*state.config).clone(),
        Keys::generate(),
        keyed(),
        state.db.clone(),
        f.bridge.clock.clone(),
    );
    f.blumint.lock().unwrap().fail = 1;
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("retrying".into(), 1)
    );
    assert_eq!(void_last(&f, &id, "void-2").await, StatusCode::CONFLICT);
    f.bridge.clock.advance(11);
    ember_bridge::deliver_results(&keyed).await;
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("delivered".into(), 2)
    );
    assert_eq!(void_last(&f, &id, "void-3").await, StatusCode::CONFLICT);
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
    // BluMint's key is configured only after these matches end.
    let f = fixture_with(Secrets::default()).await;
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

    // A match that would wait for an organizer's review has none at BluMint:
    // removing Sam's link cancels it here and frees its players.
    let (_, created) = create(&f, json!({})).await;
    let second = created["matchId"].as_str().unwrap().to_owned();
    let sam = f.sam.id().to_string();
    let link: String = f
        .bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT id FROM links WHERE ember_id = ?1 AND connection_id = ?2 AND revoked_at IS NULL",
                [sam.as_str(), CONNECTION],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap();
    let removed = f
        .bridge
        .client
        .delete(f.bridge.url(&format!("/v1/links/{link}")))
        .bearer_auth(&f.provider)
        .send()
        .await
        .unwrap();
    assert_eq!(removed.status(), StatusCode::OK);
    assert_eq!(match_status(&f, &second).await["status"], "cancelled");

    // Without the key nothing is sent; with it, both restarts are.
    ember_bridge::deliver_results(f.bridge.state()).await;
    assert!(submitted(&f).is_empty());
    let state = f.bridge.state();
    let with_key = AppState::new(
        (*state.config).clone(),
        Keys::generate(),
        keyed(),
        state.db.clone(),
        f.bridge.clock.clone(),
    );
    ember_bridge::deliver_results(&with_key).await;
    let mut sent = submitted(&f);
    sent.sort_by_key(|body| body["matchId"].as_str().unwrap().to_owned());
    let mut restarts = vec![
        json!({ "matchId": first, "mustRestart": true }),
        json!({ "matchId": second, "mustRestart": true }),
    ];
    restarts.sort_by_key(|body| body["matchId"].as_str().unwrap().to_owned());
    assert_eq!(sent, restarts);

    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (status, _) = create(&f, json!({})).await;
    assert_eq!(
        status,
        StatusCode::OK,
        "the restarted match could not be created"
    );
}

#[tokio::test]
async fn a_match_keeps_its_platforms_policy_after_the_connection_leaves_the_configuration() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, json!({})).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    // A game was permitted and neither player reported it.
    let match_id = id.clone();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO attempts (id, match_id, assignment_generation, seq, source, state, created_at, start_by, permit_id)
                 VALUES ('att_silent', ?1, 1, 1, 'player_agreement', 'permitted', 0, 0, 'pmt_silent')",
                [&match_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    // BluMint's connection leaves the configuration; then the game times out.
    let mut config = (*f.bridge.state().config).clone();
    config.tenants.retain(|tenant| tenant.id != "bm");
    let state = AppState::new(
        config,
        Keys::generate(),
        Secrets::default(),
        f.bridge.state().db.clone(),
        f.bridge.clock.clone(),
    );
    ember_bridge::sync_config(&state).await.unwrap();
    f.bridge.clock.advance(31 * 60);
    ember_bridge::maintain(&state).await;
    let match_id = id.clone();
    let (match_state, delivery): (String, String) = state
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT state, delivery_state FROM matches WHERE id = ?1",
                [&match_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?)
        })
        .await
        .unwrap();
    assert_eq!(
        (match_state.as_str(), delivery.as_str()),
        ("cancelled", "queued")
    );
}

#[tokio::test]
async fn a_disabled_connection_sends_nothing_until_enabled_again() {
    // The bridge's own worker has no key here, so only these passes send.
    let f = fixture_with(Secrets::default()).await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, json!({})).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    let (status, _) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/matches/{id}/cancel"),
            json!({ "reason": "No show", "expected_revision": status_revision(&f, &id).await }),
            Some("cancel-1"),
        )
        .await;
    assert_eq!(status, StatusCode::OK);
    // A bridge with the key, configured with the connection enabled or not.
    let keyed_state = |enabled: bool| {
        let state = f.bridge.state();
        let mut config = (*state.config).clone();
        for tenant in &mut config.tenants {
            for connection in &mut tenant.connections {
                if connection.id == CONNECTION {
                    connection.enabled = enabled;
                }
            }
        }
        AppState::new(
            config,
            Keys::generate(),
            keyed(),
            state.db.clone(),
            f.bridge.clock.clone(),
        )
    };
    let disabled = keyed_state(false);
    ember_bridge::sync_config(&disabled).await.unwrap();
    let credentials = || async {
        f.bridge
            .state()
            .db
            .read(|tx| {
                Ok(tx.query_row(
                    "SELECT COUNT(*) FROM service_credentials WHERE connection_id = ?1",
                    [CONNECTION],
                    |row| row.get::<_, i64>(0),
                )?)
            })
            .await
            .unwrap()
    };
    let issued = credentials().await;
    ember_bridge::deliver_results(&disabled).await;
    assert!(
        ember_bridge::register_blumint(&disabled, CONNECTION)
            .await
            .is_err()
    );
    // A bridge still configured with it enabled finds it disabled when leasing
    // and when registering.
    let stale = keyed_state(true);
    ember_bridge::deliver_results(&stale).await;
    assert!(
        ember_bridge::register_blumint(&stale, CONNECTION)
            .await
            .is_err()
    );
    assert!(f.blumint.lock().unwrap().calls.is_empty());
    assert_eq!(credentials().await, issued);
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("queued".into(), 0)
    );

    let enabled = keyed_state(true);
    ember_bridge::sync_config(&enabled).await.unwrap();
    ember_bridge::deliver_results(&enabled).await;
    assert_eq!(
        submitted(&f),
        vec![json!({ "matchId": id, "mustRestart": true })]
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

// BluMint's result format has no way to say a match was never played, and a
// restart would be a request it never made, so an expired match is sent
// nothing. Its status reads `cancelled` and its players can be matched again.
#[tokio::test]
async fn an_expired_match_is_not_sent_to_blumint_and_reads_as_cancelled() {
    let f = fixture().await;
    lookup(&f, json!({ "discord": [KATE, SAM] })).await;
    let (_, created) = create(&f, json!({})).await;
    let id = created["matchId"].as_str().unwrap().to_owned();
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("queued".into(), 0)
    );

    // The test clock follows the wall clock, so stay a few seconds clear of
    // the day: a second that passes during the test must not end it early.
    f.bridge.clock.advance(24 * 60 * 60 - 10);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(match_status(&f, &id).await["status"], "pending");
    // The same request while it is open still answers with the same match.
    let (_, again) = create(&f, json!({})).await;
    assert_eq!(again["matchId"], id.as_str());

    f.bridge.clock.advance(10);
    ember_bridge::maintain(f.bridge.state()).await;
    let status = match_status(&f, &id).await;
    assert_eq!(status["status"], "cancelled");
    assert_eq!(status["teams"][0]["players"][0]["status"], "absent");
    // Nothing is queued for BluMint, however many passes run.
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("not_required".into(), 0)
    );
    for _ in 0..3 {
        ember_bridge::deliver_results(f.bridge.state()).await;
    }
    assert_eq!(submitted(&f), Vec::<Json>::new());
    assert_eq!(
        results::delivery(&f.bridge, &id).await,
        ("not_required".into(), 0)
    );

    // A new request is a new match, not the expired one.
    let (status, next) = create(&f, json!({})).await;
    assert_eq!(status, StatusCode::OK, "{next}");
    assert_ne!(next["matchId"], id.as_str());
    assert_eq!(
        match_status(&f, next["matchId"].as_str().unwrap()).await["status"],
        "pending"
    );
}

// A status request without matchId gets the bridge's JSON error, and only
// after its credential is checked: a caller without one gets 401 first.
#[tokio::test]
async fn a_status_request_without_a_match_id_is_refused_as_json_after_authentication() {
    let f = fixture().await;
    let (status, body) = f
        .bridge
        .get(&f.provider, "/v1/blumint/matches/status")
        .await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{body}");
    assert_eq!(common::code(&body), "invalid_request", "{body}");
    let (status, body) = f
        .bridge
        .get("not-a-credential", "/v1/blumint/matches/status")
        .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED, "{body}");
    assert_eq!(common::code(&body), "unauthenticated", "{body}");
}
