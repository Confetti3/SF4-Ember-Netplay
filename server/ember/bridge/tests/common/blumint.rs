//! A stand-in BluMint and a bridge with a BluMint connection, for the BluMint
//! tests: BluMint's requests, what BluMint was sent, and the reports it
//! expects back.
use std::{
    collections::VecDeque,
    sync::{Arc, Mutex},
    time::Duration,
};

use axum::{
    Router,
    body::Bytes,
    extract::State,
    http::{HeaderMap, StatusCode as AxumStatus, Uri},
    response::{IntoResponse, Response},
    routing::post,
};
use ember_bridge::{
    AppState, config,
    integrations::Secrets,
    testing::{self, reconfigured, until_async},
};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use zeroize::Zeroizing;

use super::{Bridge, Player, play::Table, results, seed};

pub const KATE: &str = "274220342558756145";
pub const SAM: &str = "1725265127372152396";
pub const CONNECTION: &str = "blumint-test";
/// A second BluMint connection in the same tenant, for a test that adds it
/// (`second_connection`). BluMint has no key for it, so it is sent nothing.
pub const OTHER: &str = "blumint-other";

/// What the stand-in BluMint was sent, how many calls to refuse with 500
/// first (unrecorded, as if BluMint fell over), and the statuses to answer
/// the next recorded calls with, each with its `Retry-After` if any. Every
/// other recorded call answers 200. A report without the `idempotencyKey`
/// BluMint requires is refused with 400 and not recorded, as BluMint does.
#[derive(Default)]
pub struct Received {
    pub calls: Vec<(String, String, Json)>,
    pub fail: usize,
    pub answers: VecDeque<(u16, Option<u64>)>,
}

pub type Shared = Arc<Mutex<Received>>;

pub async fn fake_blumint() -> (String, Shared) {
    async fn record(
        State(shared): State<Shared>,
        uri: Uri,
        headers: HeaderMap,
        body: Bytes,
    ) -> Response {
        let mut received = shared.lock().unwrap();
        if received.fail > 0 {
            received.fail -= 1;
            return AxumStatus::INTERNAL_SERVER_ERROR.into_response();
        }
        let body: Json = serde_json::from_slice(&body).unwrap();
        if uri.path() == "/api/tournaments/match/submit" && !creation_key(&body) {
            return (
                AxumStatus::BAD_REQUEST,
                [("content-type", "application/json")],
                r#"{ "code": "invalid_request", "message": "idempotencyKey is required." }"#,
            )
                .into_response();
        }
        let key = headers
            .get("x-api-key")
            .and_then(|v| v.to_str().ok())
            .unwrap_or("")
            .to_owned();
        received.calls.push((uri.path().to_owned(), key, body));
        let Some((status, retry_after)) = received.answers.pop_front() else {
            return AxumStatus::OK.into_response();
        };
        let mut response = (
            AxumStatus::from_u16(status).unwrap(),
            [("content-type", "application/json")],
            r#"{ "code": "scripted", "message": "A scripted answer." }"#,
        )
            .into_response();
        if let Some(seconds) = retry_after {
            response
                .headers_mut()
                .insert("retry-after", seconds.to_string().parse().unwrap());
        }
        response
    }
    let shared = Shared::default();
    let app = Router::new()
        .route("/api/tournaments/match/submit", post(record))
        .route("/api/tournaments/setLookupPlayer", post(record))
        .route("/api/tournaments/match/setCreate", post(record))
        .route("/api/tournaments/match/setClose", post(record))
        .with_state(shared.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let base = format!("http://{}/api", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await });
    (base, shared)
}

/// Whether a report carries a creation key in the form BluMint gives one.
fn creation_key(body: &Json) -> bool {
    body["idempotencyKey"].as_str().is_some_and(|key| {
        (1..=128).contains(&key.len())
            && key.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
    })
}

pub struct Fixture {
    pub bridge: Bridge,
    pub blumint: Shared,
    pub provider: String,
    pub organizer: String,
    pub kate: Player,
    pub sam: Player,
}

impl Table for Fixture {
    fn bridge(&self) -> &Bridge {
        &self.bridge
    }

    fn a(&self) -> &Player {
        &self.kate
    }

    fn b(&self) -> &Player {
        &self.sam
    }
}

pub fn keyed() -> Secrets {
    Secrets {
        api_keys: [(CONNECTION.to_owned(), Zeroizing::new("bm-key".to_owned()))].into(),
        ..Default::default()
    }
}

pub async fn fixture() -> Fixture {
    fixture_with(keyed(), config::DEFAULT_MATCH_EXPIRY_HOURS).await
}

/// The fixture with these integration secrets, its quiet matches expiring
/// after `match_expiry_hours`.
pub async fn fixture_with(secrets: Secrets, match_expiry_hours: u64) -> Fixture {
    fixture_adjusted(secrets, match_expiry_hours, |_| {}).await
}

/// `fixture_with`, its configuration changed by `adjust` once the BluMint
/// tenant is in it.
pub async fn fixture_adjusted(
    secrets: Secrets,
    match_expiry_hours: u64,
    adjust: impl FnOnce(&mut config::Config),
) -> Fixture {
    let (api_base, blumint) = fake_blumint().await;
    let bridge = Bridge::start_with(
        |config| {
            config.match_expiry_hours = match_expiry_hours;
            config.tenants.push(config::Tenant {
                id: "bm".into(),
                name: "BluMint".into(),
                connections: vec![connection(CONNECTION, Some(api_base))],
            });
            adjust(config);
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
        seed::link_discord(bridge.state(), user_id, player.id().as_str(), name).await;
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

/// Another player, with a session, whose Ember ID is connected to Discord
/// account `user_id`, for a test that needs a second pair of fighters.
pub async fn discord_player(f: &Fixture, byte: u8, user_id: &str, name: &str) -> Player {
    let mut player = f.bridge.player(byte);
    f.bridge.open_session(&mut player).await;
    seed::link_discord(f.bridge.state(), user_id, player.id().as_str(), name).await;
    player
}

fn connection(id: &str, api_base: Option<String>) -> config::Connection {
    config::Connection {
        kind: "blumint".into(),
        environment: "staging".into(),
        api_base,
        ..testing::connection(id, "BluMint (test)")
    }
}

/// Adds `OTHER` to the BluMint tenant (`fixture_adjusted`).
pub fn second_connection(config: &mut config::Config) {
    let tenant = config
        .tenants
        .iter_mut()
        .find(|tenant| tenant.id == "bm")
        .unwrap();
    tenant.connections.push(connection(OTHER, None));
}

pub async fn lookup(f: &Fixture, body: Json) -> Json {
    let (status, found) = f.bridge.post(&f.provider, "/v1/blumint/lookup", body).await;
    assert_eq!(status, StatusCode::OK, "{found}");
    found
}

/// One team of one player, known to BluMint by the Ember ID its lookup found.
pub fn team(name: &str, ember_id: impl serde::Serialize) -> Json {
    json!({ "name": name, "players": [ { "identities": [ { "inGameId": ember_id } ] } ] })
}

/// BluMint's request for a set of Kate against Sam, first to `games_to_win`.
pub fn request(f: &Fixture, games_to_win: u8) -> Json {
    json!({
        "teams": [team("kate", f.kate.id()), team("sam", f.sam.id())],
        "spectators": [],
        "matchSettings": {},
        "series": { "gamesToWin": games_to_win },
    })
}

/// BluMint's request under its `Idempotency-Key` `key`.
pub async fn create(f: &Fixture, key: &str, games_to_win: u8) -> (StatusCode, Json) {
    create_with(f, key, request(f, games_to_win)).await
}

pub async fn create_with(f: &Fixture, key: &str, body: Json) -> (StatusCode, Json) {
    f.bridge
        .post_keyed(&f.provider, "/v1/blumint/matches", body, Some(key))
        .await
}

/// The registry code of a refusal. BluMint reads it at the top level only,
/// so the bridge's own `error` envelope must not be there instead.
pub fn refusal(body: &Json) -> &str {
    assert!(body.get("error").is_none(), "{body}");
    body["code"].as_str().unwrap_or("")
}

pub async fn matches(f: &Fixture) -> i64 {
    f.bridge
        .state()
        .db
        .read(|tx| Ok(tx.query_row("SELECT COUNT(*) FROM matches", [], |row| row.get(0))?))
        .await
        .unwrap()
}

/// Each report on a set in play queued for the match: games played, state.
pub async fn progress(f: &Fixture, id: &str) -> Vec<(u64, String)> {
    let id = id.to_owned();
    f.bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx
                .prepare("SELECT games_played, state FROM blumint_progress WHERE match_id = ?1 ORDER BY games_played")?
                .query_map([&id], |row| Ok((row.get(0)?, row.get(1)?)))?
                .collect::<rusqlite::Result<Vec<_>>>()?)
        })
        .await
        .unwrap()
}

/// The match's status report as the bridge keeps it: its revision and its
/// delivery state.
pub async fn status_row(f: &Fixture, id: &str) -> Option<(u64, String)> {
    let id = id.to_owned();
    f.bridge
        .state()
        .db
        .read(move |tx| {
            use rusqlite::OptionalExtension;
            Ok(tx
                .query_row(
                    "SELECT revision, state FROM blumint_status WHERE match_id = ?1",
                    [&id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?)
        })
        .await
        .unwrap()
}

/// Runs delivery passes until no report on the match waits to be sent now,
/// for at most five seconds, and returns what BluMint was sent for it other
/// than its status. The bridge's own workers run alongside; any of them may
/// send.
pub async fn reports(f: &Fixture, id: &str) -> Vec<Json> {
    settle(f, id).await;
    sent_for(f, id, |body| body["mode"] != "status")
}

/// The status reports BluMint was sent for the match so far, in the order
/// they arrived.
pub fn statuses(f: &Fixture, id: &str) -> Vec<Json> {
    sent_for(f, id, |body| body["mode"] == "status")
}

/// A status report on the match without the time it was observed, which
/// follows the test clock, and that time.
pub fn untimed(mut status: Json) -> (Json, String) {
    let at = status
        .as_object_mut()
        .and_then(|body| body.remove("observedAt"))
        .and_then(|at| at.as_str().map(str::to_owned))
        .unwrap_or_default();
    (status, at)
}

/// The status report of the match's `revision`, with Kate's and Sam's
/// presence.
pub fn status(f: &Fixture, id: &str, revision: u64, status: &str, presence: [&str; 2]) -> Json {
    json!({
        "mode": "status",
        "matchId": id,
        "revision": revision,
        "status": status,
        "players": [
            { "inGameId": f.kate.id(), "status": presence[0] },
            { "inGameId": f.sam.id(), "status": presence[1] },
        ],
    })
}

fn sent_for(f: &Fixture, id: &str, keep: impl Fn(&Json) -> bool) -> Vec<Json> {
    sent_keyed(f, id)
        .into_iter()
        .map(|(_, body)| body)
        .filter(keep)
        .collect()
}

/// The creation key each report BluMint was sent for the match carried,
/// status reports included, in the order they arrived.
pub fn report_keys(f: &Fixture, id: &str) -> Vec<(String, String)> {
    sent_keyed(f, id)
        .into_iter()
        .map(|(key, body)| (body["mode"].as_str().unwrap().to_owned(), key))
        .collect()
}

/// Each report BluMint was sent for the match, apart from its creation key,
/// which the stand-in BluMint has checked is there and `report_keys` reads.
fn sent_keyed(f: &Fixture, id: &str) -> Vec<(String, Json)> {
    let received = f.blumint.lock().unwrap();
    received
        .calls
        .iter()
        .filter(|(path, key, body)| {
            path == "/api/tournaments/match/submit" && key == "bm-key" && body["matchId"] == id
        })
        .map(|(_, _, body)| unkeyed(body.clone()))
        .collect()
}

/// A report without its creation key, and that key.
fn unkeyed(mut body: Json) -> (String, Json) {
    let key = body
        .as_object_mut()
        .and_then(|body| body.remove("idempotencyKey"))
        .and_then(|key| key.as_str().map(str::to_owned))
        .unwrap_or_default();
    (key, body)
}

async fn settle(f: &Fixture, id: &str) {
    until_async("the reports to settle", Duration::from_secs(10), || async {
        let (match_id, now) = (id.to_owned(), f.bridge.clock.now());
        let due: bool = f
            .bridge
            .state()
            .db
            .read(move |tx| {
                Ok(tx.query_row(
                    "SELECT EXISTS (SELECT 1 FROM blumint_progress p JOIN matches m ON m.id = p.match_id
                       WHERE p.match_id = ?1 AND m.state NOT IN ('completed', 'cancelled', 'failed', 'expired')
                         AND (p.state IN ('queued', 'delivering') OR (p.state = 'retrying' AND p.next_at <= ?2)))
                     OR EXISTS (SELECT 1 FROM blumint_status s JOIN matches m ON m.id = s.match_id
                       WHERE s.match_id = ?1 AND m.state NOT IN ('completed', 'cancelled', 'failed', 'expired')
                         AND (s.state IN ('queued', 'delivering') OR (s.state = 'retrying' AND s.next_at <= ?2)))
                     OR EXISTS (SELECT 1 FROM matches WHERE id = ?1 AND state IN ('completed', 'cancelled', 'failed', 'expired')
                         AND (delivery_state IN ('queued', 'delivering')
                           OR (delivery_state = 'retrying' AND delivery_next_at <= ?2)))",
                    rusqlite::params![match_id, now],
                    |row| row.get(0),
                )?)
            })
            .await
            .unwrap();
        if !due {
            return true;
        }
        ember_bridge::deliver_results(f.bridge.state()).await;
        false
    })
    .await;
}

/// Kate's and Sam's teams with their wins, as every report carries them.
pub fn teams(f: &Fixture, wins: [u8; 2]) -> Json {
    json!([
        { "players": [ { "inGameId": f.kate.id() } ], "wins": wins[0] },
        { "players": [ { "inGameId": f.sam.id() } ], "wins": wins[1] },
    ])
}

/// An abort BluMint can ask the set again after, with the wins so far.
pub fn restart(f: &Fixture, id: &str, reason: &str, wins: [u8; 2]) -> Json {
    json!({ "mode": "abort", "matchId": id, "reason": reason, "retryable": true, "teams": teams(f, wins) })
}

/// Every report BluMint was sent other than a status, which `statuses`
/// reads.
pub fn submitted(f: &Fixture) -> Vec<Json> {
    let received = f.blumint.lock().unwrap();
    received
        .calls
        .iter()
        .filter(|(path, key, body)| {
            path == "/api/tournaments/match/submit" && key == "bm-key" && body["mode"] != "status"
        })
        .map(|(_, _, body)| unkeyed(body.clone()).1)
        .collect()
}

pub async fn win(f: &Fixture, id: &str, slot: u8, key: &str) {
    results::win(&f.bridge, &f.provider, &f.organizer, id, slot, key).await;
}

pub async fn void_last(f: &Fixture, id: &str, key: &str) -> StatusCode {
    results::void_last(&f.bridge, &f.provider, &f.organizer, id, key)
        .await
        .0
}

/// A match the bridge's own API made for two other players on `mock-a`,
/// another connection: the connection's provider credential, and the match.
pub async fn direct_match(f: &Fixture) -> (String, String) {
    let provider = f.bridge.provider("mock-a").await;
    let (mut a, mut b) = (f.bridge.player(41), f.bridge.player(42));
    f.bridge.open_session(&mut a).await;
    f.bridge.open_session(&mut b).await;
    let link_a = f.bridge.link(&provider, &a, "mock-a", "direct-a").await;
    let link_b = f.bridge.link(&provider, &b, "mock-a", "direct-b").await;
    let (status, direct) = f
        .bridge
        .post_keyed(
            &provider,
            "/v1/matches",
            json!({
                "external_match_id": "direct-1",
                "game": "usf4",
                "participants": [
                    { "participant_id": link_a["participant_id"], "ember_id": a.id(), "slot": 0 },
                    { "participant_id": link_b["participant_id"], "ember_id": b.id(), "slot": 1 },
                ],
                "rules": {
                    "games_to_win": 2, "draw_policy": "replay_no_score", "native_rules_profile": "organizer-reported-v1",
                    "edition_policy": "ultra_only", "character_policy": "unrestricted_between_games",
                    "stage_policy": "p1_selects", "input_delay_policy": "ember_existing_ready_policy",
                },
                "observer_policy": "authorized_only",
                "result_policy": "two_player_agreement_or_review",
                "required_build_id": "test-build",
                "metadata": {},
            }),
            Some("direct-1"),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{direct}");
    let id = direct["match_id"].as_str().unwrap().to_owned();
    (provider, id)
}

/// The organizer voids the match's `nth` accepted game, first game 0.
pub async fn void_game(f: &Fixture, id: &str, nth: usize, key: &str) -> StatusCode {
    let (_, found) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    let attempt = found["attempts"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|attempt| attempt["state"] == "accepted")
        .nth(nth)
        .unwrap()["attempt_id"]
        .clone();
    f.bridge
        .post_keyed(
            &f.organizer,
            &format!("/v1/matches/{id}/adjudications"),
            json!({ "kind": "void_game", "attempt_id": attempt, "reason": "Wrong game", "expected_revision": found["revision"] }),
            Some(key),
        )
        .await
        .0
}

/// A bridge on the fixture's database with BluMint's key, for a test whose
/// own bridge has none, so only the delivery passes it runs send.
pub fn keyed_state(f: &Fixture) -> AppState {
    reconfigured(f.bridge.state(), |_| {}, keyed())
}

/// The match's state, as the bridge's own match route reads it.
pub async fn match_state(f: &Fixture, id: &str) -> String {
    let (status, found) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    assert_eq!(status, StatusCode::OK, "{found}");
    found["state"].as_str().unwrap().to_owned()
}

pub async fn status_revision(f: &Fixture, id: &str) -> String {
    let (_, found) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    found["revision"].as_str().unwrap().to_owned()
}

/// The organizer records a drawn game, which changes no score.
pub async fn draw(f: &Fixture, id: &str, key: &str) {
    let (_, found) = f
        .bridge
        .get(&f.provider, &format!("/v1/matches/{id}"))
        .await;
    let (status, decided) = f
        .bridge
        .post_keyed(
            &f.organizer,
            &format!("/v1/matches/{id}/adjudications"),
            json!({ "kind": "game_result", "draw": true, "reason": "test", "expected_revision": found["revision"] }),
            Some(key),
        )
        .await;
    assert_eq!(status, StatusCode::CREATED, "{decided}");
}

/// The provider calls the match off.
pub async fn cancel(f: &Fixture, id: &str, key: &str) {
    let (status, cancelled) = f
        .bridge
        .post_keyed(
            &f.provider,
            &format!("/v1/matches/{id}/cancel"),
            json!({ "reason": "No show", "expected_revision": status_revision(f, id).await }),
            Some(key),
        )
        .await;
    assert_eq!(status, StatusCode::OK, "{cancelled}");
}

pub fn progressed(f: &Fixture, id: &str, wins: [u8; 2]) -> Json {
    json!({ "mode": "progress", "matchId": id, "teams": teams(f, wins) })
}

pub fn result(f: &Fixture, id: &str, wins: [u8; 2]) -> Json {
    json!({ "mode": "result", "matchId": id, "teams": teams(f, wins) })
}

/// When the match's result is next tried.
pub async fn next_attempt(f: &Fixture, id: &str) -> u64 {
    let id = id.to_owned();
    f.bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT delivery_next_at FROM matches WHERE id = ?1",
                [&id],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap()
}

/// BluMint's close, under a close key as long as BluMint's longest.
pub async fn close(f: &Fixture, body: Json) -> (StatusCode, Json) {
    let key = format!("close-key-{}", "k".repeat(128));
    f.bridge
        .post_keyed(&f.provider, "/v1/blumint/matches/close", body, Some(&key))
        .await
}

/// BluMint's request for a set it asks for again, from `wins`.
pub fn carried(f: &Fixture, games_to_win: u8, wins: [u8; 2]) -> Json {
    let mut body = request(f, games_to_win);
    for (slot, won) in wins.into_iter().enumerate() {
        body["teams"][slot]["startingWins"] = json!(won);
    }
    body
}
