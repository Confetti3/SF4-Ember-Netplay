//! The public replay archive (docs/design/REPLAY_ARCHIVE.md): any game a
//! player sends, and every tournament set's game, kept once by its replay's
//! digest, stored in the bucket with the set's pack, then listed for the
//! archive page.
mod common;

use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};

use common::{
    Bridge, Player,
    play::{Table, bound, create_match, permit, played, replay_of, report},
};
use ember_protocol::{
    replays::{self, HEAD, Replay},
    report::Outcome,
};
use reqwest::StatusCode;
use serde_json::{Value as Json, json};
use sha2::{Digest, Sha256};

const DAY: i64 = 24 * 60 * 60;

/// What the stand-in bucket was sent: each PUT's path and body.
type Stored = Arc<Mutex<Vec<(String, Vec<u8>)>>>;

/// An S3 stand-in that takes any signed PUT whose content hash is right,
/// unless it is `refusing`.
async fn bucket(refusing: Arc<AtomicBool>) -> (String, Stored) {
    let stored: Stored = Arc::default();
    let keep = stored.clone();
    let app = axum::Router::new().fallback(
        move |method: axum::http::Method,
              uri: axum::http::Uri,
              headers: axum::http::HeaderMap,
              body: axum::body::Bytes| {
            let (keep, refusing) = (keep.clone(), refusing.clone());
            async move {
                let hash: String = Sha256::digest(&body)
                    .iter()
                    .map(|b| format!("{b:02x}"))
                    .collect();
                let signed = headers
                    .get("authorization")
                    .and_then(|v| v.to_str().ok())
                    .is_some_and(|v| v.starts_with("AWS4-HMAC-SHA256 Credential=ember-test/"));
                if refusing.load(Ordering::SeqCst)
                    || method != axum::http::Method::PUT
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
    refusing: Arc<AtomicBool>,
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
    fixture_with(|_| {}).await
}

/// An archive bridge whose configuration `adjust` changes first.
async fn fixture_with(adjust: impl FnOnce(&mut ember_bridge::Config) + Send + 'static) -> Fixture {
    let refusing = Arc::new(AtomicBool::new(false));
    let (endpoint, stored) = bucket(refusing.clone()).await;
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
            adjust(config);
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
        refusing,
    }
}

/// An online game `n` with fighters `fighters` (native IDs).
fn game(n: u8, fighters: [u8; 2]) -> Vec<u8> {
    replays::made_up(&[n; 64], 1_800_000_000, fighters)
}

/// `file` as another PC saved it: in slot 284, so only its replay is the
/// same.
fn saved_elsewhere(file: &[u8]) -> Vec<u8> {
    let mut copy = file.to_vec();
    copy[8..12].copy_from_slice(&284u32.to_le_bytes());
    copy
}

fn digest(file: &[u8]) -> String {
    Replay::parse(file).unwrap().digest_hex()
}

/// `file` with the last four bytes of its replay changed so that the
/// replay's CRC-32, which is linear in its bits, is `crc`.
fn with_crc(mut file: Vec<u8>, crc: u32) -> Vec<u8> {
    let patch = file.len() - 4;
    let mut crc_with = |value: u32| {
        file[patch..].copy_from_slice(&value.to_le_bytes());
        replays::crc32(&file[HEAD..])
    };
    let zero = crc_with(0);
    // Each patch bit's effect on the CRC, reduced to one row per top bit.
    let mut rows = [(0u32, 0u32); 32];
    for bit in 0..32 {
        let (mut effect, mut value) = (crc_with(1 << bit) ^ zero, 1u32 << bit);
        while effect != 0 {
            let top = 31 - effect.leading_zeros() as usize;
            if rows[top].0 == 0 {
                rows[top] = (effect, value);
                break;
            }
            effect ^= rows[top].0;
            value ^= rows[top].1;
        }
    }
    let (mut wanted, mut value) = (crc ^ zero, 0);
    while wanted != 0 {
        let top = 31 - wanted.leading_zeros() as usize;
        wanted ^= rows[top].0;
        value ^= rows[top].1;
    }
    assert_eq!(crc_with(value), crc);
    file[8 + 5..8 + 9].copy_from_slice(&crc.to_le_bytes());
    file
}

async fn send(f: &Fixture, sender: &Player, replay: &[u8], query: &str) -> StatusCode {
    let crc = replays::crc_name(replay).unwrap();
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

/// `sender` sends a game of set `id` played in its first room, saying
/// `query` of its sides.
async fn send_to_set(
    f: &Fixture,
    sender: &Player,
    id: &str,
    replay: &[u8],
    query: &str,
) -> StatusCode {
    set_answer(f, sender, id, replay, &format!("revision=1&{query}"))
        .await
        .0
}

/// `send_to_set` with the whole query `query`, and the bridge's answer.
async fn set_answer(
    f: &Fixture,
    sender: &Player,
    id: &str,
    replay: &[u8],
    query: &str,
) -> (StatusCode, Json) {
    let crc = replays::crc_name(replay).unwrap();
    let response = f
        .bridge
        .client
        .put(
            f.bridge
                .url(&format!("/v1/matches/{id}/replays/{crc}?{query}")),
        )
        .bearer_auth(sender.token())
        .body(replay.to_vec())
        .send()
        .await
        .unwrap();
    let status = response.status();
    let body = response.bytes().await.unwrap();
    (status, serde_json::from_slice(&body).unwrap_or(Json::Null))
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

/// Waits until `done` holds of the listing for `query`, and returns it.
async fn listed_when(f: &Fixture, query: &str, done: impl Fn(&Json) -> bool) -> Json {
    for _ in 0..200 {
        let list = listed(f, query).await;
        if done(&list) {
            return list;
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    panic!("the listing is {}", listed(f, query).await);
}

fn games(list: &Json) -> &Vec<Json> {
    list["games"].as_array().unwrap()
}

/// The listed game whose file is `file`'s.
fn listed_game<'a>(list: &'a Json, file: &[u8]) -> &'a Json {
    let digest = digest(file);
    games(list)
        .iter()
        .find(|game| game["digest"] == digest.as_str())
        .unwrap_or_else(|| panic!("{digest} is not in {list}"))
}

/// The games of the last pack of set `id` the bucket took, by CRC.
fn stored_pack(f: &Fixture, id: &str) -> Option<Vec<Json>> {
    let stored = f.stored.lock().unwrap();
    let (_, pack) = stored
        .iter()
        .rev()
        .find(|(path, _)| *path == format!("/replays/sets/{id}.emberpack"))?;
    let length = u32::from_le_bytes(pack[8..12].try_into().unwrap()) as usize;
    let header: Json = serde_json::from_slice(&pack[12..12 + length]).unwrap();
    Some(header["games"].as_array().unwrap().clone())
}

async fn sql(f: &Fixture, statement: &'static str, values: Vec<String>) {
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(statement, rusqlite::params_from_iter(values))?;
            Ok(())
        })
        .await
        .unwrap();
}

async fn count(f: &Fixture, query: &'static str, value: &str) -> i64 {
    let value = value.to_owned();
    f.bridge
        .state()
        .db
        .read(move |tx| Ok(tx.query_row(query, [value], |row| row.get(0))?))
        .await
        .unwrap()
}

/// Waits until the bucket holds set `id`'s pack at `version`, an hour of
/// the bridge's clock passing each look, so retries the archive put off
/// hourly come due.
async fn pack_stored(f: &Fixture, id: &str, version: i64) {
    let stored = "SELECT COALESCE(MAX(uploaded_version), 0) FROM archive_sets WHERE match_id = ?1";
    for _ in 0..200 {
        if count(f, stored, id).await >= version {
            return;
        }
        f.bridge.clock.advance(3600);
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    panic!("the pack of {id} is at {}", count(f, stored, id).await);
}

async fn set(f: &Fixture, external: &str, games: u8) -> String {
    let id = create_match(
        &f.bridge,
        &f.provider,
        &f.links,
        [&f.a, &f.b],
        external,
        "ember-room-v1",
        2,
    )
    .await;
    played(f, &id, games).await;
    id
}

/// The first copy of an online game says what it was; another player's
/// copy of it, or a downloaded copy sent again, changes nothing.
#[tokio::test]
async fn an_online_game_is_kept_once_as_its_first_copy_said() {
    let f = fixture().await;
    let played = game(7, [20, 34]);
    assert_eq!(
        send(&f, &f.a, &played, "a=Ann&b=Bob&side=0&country=BE").await,
        StatusCode::CREATED
    );
    assert_eq!(
        send(
            &f,
            &f.b,
            &saved_elsewhere(&played),
            "a=Eve&b=Bob&side=1&country=NL"
        )
        .await,
        StatusCode::OK
    );
    assert_eq!(
        send(&f, &f.b, &played, "a=Eve&b=Mallory&side=1&country=JP").await,
        StatusCode::OK
    );
    let mut torn = game(8, [0, 0]);
    torn.pop();
    assert_eq!(send(&f, &f.a, &torn, "").await, StatusCode::BAD_REQUEST);
    // A file whose record does not describe its replay is none.
    let mut misnamed = game(9, [3, 4]);
    misnamed[8 + 4] = 0;
    assert_eq!(send(&f, &f.a, &misnamed, "").await, StatusCode::BAD_REQUEST);
    let anonymous = f
        .bridge
        .client
        .put(f.bridge.url("/v1/replays/00000000"))
        .body(played.clone())
        .send()
        .await
        .unwrap();
    assert_eq!(anonymous.status(), StatusCode::UNAUTHORIZED);
    let list = listed_when(&f, "", |list| !games(list).is_empty()).await;
    let digest = digest(&played);
    assert_eq!(
        f.stored.lock().unwrap().clone(),
        vec![(
            format!("/replays/games/{digest}.emberreplay"),
            played.clone()
        )]
    );
    let listed_game = &games(&list)[0];
    assert_eq!(listed_game["names"], json!(["Ann", "Bob"]));
    assert_eq!(listed_game["countries"], json!(["BE", null]));
    assert_eq!(listed_game["fighters"], json!([20, 34]));
    assert_eq!(listed_game["crc"], replays::crc_name(&played).unwrap());
    assert_eq!(
        listed_game["url"],
        format!("https://s3.example/replays/games/{digest}.emberreplay")
    );
    assert_eq!(list["countries"], json!(["BE"]));
    assert_eq!((&list["more"], &list["next"]), (&json!(false), &Json::Null));
    // Stored, the replay leaves the bridge.
    let held = count(
        &f,
        "SELECT COUNT(replay) FROM archive_games WHERE digest = ?1",
        &digest,
    )
    .await;
    assert_eq!(held, 0);
    for (query, games_kept) in [
        ("fighter=34", 1),
        ("fighter=1", 0),
        ("country=BE", 1),
        ("country=NL", 0),
        ("q=an", 1),
        ("event=online", 1),
        ("round=top8", 0),
    ] {
        assert_eq!(games(&listed(&f, query).await).len(), games_kept, "{query}");
    }
    let response = f
        .bridge
        .client
        .get(f.bridge.url("/v1/archive?cursor=12"))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::BAD_REQUEST);
}

/// Two games whose replays have one CRC are two games, each stored.
#[tokio::test]
async fn games_whose_crcs_collide_are_both_kept() {
    let f = fixture().await;
    let first = game(1, [5, 6]);
    let crc = Replay::parse(&first).unwrap().crc;
    let second = with_crc(game(2, [5, 6]), crc);
    assert_ne!(first, second);
    assert_eq!(replays::crc_name(&second), replays::crc_name(&first));
    assert_eq!(send(&f, &f.a, &first, "").await, StatusCode::CREATED);
    assert_eq!(send(&f, &f.b, &second, "").await, StatusCode::CREATED);
    let list = listed_when(&f, "", |list| games(list).len() == 2).await;
    assert_ne!(
        listed_game(&list, &first)["url"],
        listed_game(&list, &second)["url"]
    );
    let mut stored: Vec<Vec<u8>> = f
        .stored
        .lock()
        .unwrap()
        .iter()
        .map(|(_, body)| body.clone())
        .collect();
    stored.sort();
    let mut sent = vec![first, second];
    sent.sort();
    assert_eq!(stored, sent);
}

/// A set's games are sided by its roster, whatever its players' games say,
/// and each player adds only their own country. Its pack in the bucket
/// ends up with every game the set gained.
#[tokio::test]
async fn a_sets_games_are_sided_by_its_roster_and_packed_whole() {
    let f = fixture().await;
    let id = set(&f, "archive-set", 2).await;
    for (sender, n, query, status) in [
        (
            &f.a,
            1,
            "a=Mallory&b=Mallory&side=1&country=BE",
            StatusCode::CREATED,
        ),
        (&f.b, 1, "side=0&country=NL", StatusCode::OK),
        (&f.a, 2, "country=BE", StatusCode::CREATED),
    ] {
        assert_eq!(
            send_to_set(&f, sender, &id, &replay_of(&f.bridge, n), query).await,
            status
        );
    }
    let list = listed_when(&f, "", |list| {
        games(list).len() == 2 && list["sets"][&id]["pack"].is_string()
    })
    .await;
    for n in [1, 2] {
        let listed = listed_game(&list, &replay_of(&f.bridge, n));
        assert_eq!(listed["set"], id.as_str());
        assert_eq!(listed["names"], json!([null, null]));
    }
    assert_eq!(
        listed_game(&list, &replay_of(&f.bridge, 1))["countries"],
        json!(["BE", "NL"])
    );
    assert_eq!(
        listed_game(&list, &replay_of(&f.bridge, 2))["countries"],
        json!(["BE", null])
    );
    assert_eq!(
        list["sets"][&id]["pack"],
        format!("https://s3.example/replays/sets/{id}.emberpack")
    );
    assert_eq!(games(&listed(&f, "event=online").await).len(), 0);
    // The pack catches up with the set's second game, then rests.
    let settled = "SELECT COUNT(*) FROM archive_sets WHERE match_id = ?1 AND version = 2 AND uploaded_version = 2";
    for _ in 0..200 {
        if count(&f, settled, &id).await == 1 {
            break;
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    assert_eq!(count(&f, settled, &id).await, 1);
    assert_eq!(stored_pack(&f, &id).unwrap().len(), 2);
}

/// An online game joins a set only when its own sender sends it for the
/// set, and then the set's pack is stored though the game already was.
#[tokio::test]
async fn an_online_game_joins_a_set_only_from_its_own_sender() {
    let f = fixture().await;
    let id = set(&f, "archive-joined", 2).await;
    assert_eq!(
        send(&f, &f.b, &replay_of(&f.bridge, 1), "a=Ann&b=Bob").await,
        StatusCode::CREATED
    );
    assert_eq!(
        send(&f, &f.a, &replay_of(&f.bridge, 2), "a=Ann&b=Bob").await,
        StatusCode::CREATED
    );
    listed_when(&f, "", |list| games(list).len() == 2).await;
    for n in [1, 2] {
        assert_eq!(
            send_to_set(&f, &f.a, &id, &replay_of(&f.bridge, n), "").await,
            StatusCode::CREATED
        );
    }
    let list = listed_when(&f, "", |list| list["sets"][&id]["pack"].is_string()).await;
    assert_eq!(
        listed_game(&list, &replay_of(&f.bridge, 1))["set"],
        Json::Null
    );
    assert_eq!(
        listed_game(&list, &replay_of(&f.bridge, 1))["names"],
        json!(["Ann", "Bob"])
    );
    assert_eq!(
        listed_game(&list, &replay_of(&f.bridge, 2))["set"],
        id.as_str()
    );
    assert_eq!(
        listed_game(&list, &replay_of(&f.bridge, 2))["names"],
        json!([null, null])
    );
    assert_eq!(list["events"], json!([]));
}

/// A set takes a replay only as one of its confirmed games, saved in that
/// game's own time, and each game only once: not one saved while the set
/// was open between its games, nor one saved long before or after it, nor
/// a third for a set of two games, nor one from a room the set never had.
#[tokio::test]
async fn a_replay_is_kept_only_as_one_confirmed_game() {
    let mut f = fixture().await;
    let id = create_match(
        &f.bridge,
        &f.provider,
        &f.links,
        [&f.a, &f.b],
        "archive-games",
        "ember-room-v1",
        2,
    )
    .await;
    // The set's two games are played ten minutes after it opens.
    f.bridge.clock.advance(600);
    f.bridge.open_session(&mut f.a).await;
    f.bridge.open_session(&mut f.b).await;
    played(&f, &id, 2).await;
    let now = f.bridge.clock.now() as u32;
    let saved = |n: u8, at: u32| replays::wrap(&[n; 64], at);
    for other in [
        saved(9, now - 400),
        saved(8, now - 2 * DAY as u32),
        saved(7, now + 2 * DAY as u32),
    ] {
        let (status, refused) = set_answer(&f, &f.a, &id, &other, "revision=1").await;
        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert!(
            refused.to_string().contains("saved in no game"),
            "{refused}"
        );
    }
    let (status, refused) = set_answer(&f, &f.a, &id, &saved(1, now), "revision=9").await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    assert!(
        refused.to_string().contains("no room with that revision"),
        "{refused}"
    );
    for (n, status) in [
        (1, StatusCode::CREATED),
        (2, StatusCode::CREATED),
        (3, StatusCode::BAD_REQUEST),
    ] {
        assert_eq!(send_to_set(&f, &f.b, &id, &saved(n, now), "").await, status);
    }
    let bound = count(
        &f,
        "SELECT COUNT(DISTINCT attempt_id) FROM match_replays WHERE match_id = ?1",
        &id,
    )
    .await;
    assert_eq!(bound, 2);
}

/// While 2 GiB of games wait for the bucket, neither route takes a new
/// game, for now; the set keeps nothing either.
#[tokio::test]
async fn both_routes_wait_while_the_bucket_is_behind() {
    let f = fixture().await;
    let id = set(&f, "archive-full", 1).await;
    sql(
        &f,
        "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, sent_by, priority, replay,
            next_attempt_at, received_at)
         VALUES (?1, '00000000', 1, 2147483648, 1, 2, 'emb_other', 1, x'00', 9223372036854775807, 1)",
        vec!["f".repeat(64)],
    )
    .await;
    assert_eq!(
        send(&f, &f.a, &game(1, [1, 2]), "").await,
        StatusCode::SERVICE_UNAVAILABLE
    );
    assert_eq!(
        send_to_set(&f, &f.a, &id, &replay_of(&f.bridge, 1), "").await,
        StatusCode::SERVICE_UNAVAILABLE
    );
    let held = count(
        &f,
        "SELECT COUNT(*) FROM match_replays WHERE match_id = ?1",
        &id,
    )
    .await;
    assert_eq!(held, 0);
    sql(
        &f,
        "DELETE FROM archive_games WHERE digest = ?1",
        vec!["f".repeat(64)],
    )
    .await;
    assert_eq!(
        send_to_set(&f, &f.a, &id, &replay_of(&f.bridge, 1), "").await,
        StatusCode::CREATED
    );
}

/// A player's day of 3000 online games stops their online games, not their
/// sets'; and a set's game is no online game of the day.
#[tokio::test]
async fn a_senders_day_counts_only_their_online_games() {
    let f = fixture().await;
    let id = set(&f, "archive-day", 1).await;
    let (sender, now) = (f.a.id().as_str().to_owned(), f.bridge.clock.now());
    f.bridge
        .state()
        .db
        .write(move |tx| {
            for n in 0..2999 {
                tx.execute(
                    "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, sent_by, charged,
                        priority, state, next_attempt_at, received_at)
                     VALUES (?1, '00000000', 1, 1, 1, 2, ?2, 1, 1, 'stored', 0, ?3)",
                    rusqlite::params![format!("{n:064x}"), sender, now],
                )?;
            }
            Ok(())
        })
        .await
        .unwrap();
    assert_eq!(
        send_to_set(&f, &f.a, &id, &replay_of(&f.bridge, 1), "").await,
        StatusCode::CREATED
    );
    assert_eq!(
        send(&f, &f.a, &game(1, [1, 2]), "").await,
        StatusCode::CREATED
    );
    assert_eq!(
        send(&f, &f.a, &game(2, [1, 2]), "").await,
        StatusCode::TOO_MANY_REQUESTS
    );
    assert_eq!(
        send(&f, &f.b, &game(2, [1, 2]), "").await,
        StatusCode::CREATED
    );
}

/// A set's games kept before the archive was on go in as the set's: one
/// that a player sends again, and the rest by the backfill.
#[tokio::test]
async fn sets_kept_before_the_archive_are_archived_as_sets() {
    let f = fixture().await;
    let id = set(&f, "archive-before", 2).await;
    let kept = "INSERT INTO match_replays (match_id, crc, revision, saved_at, sent_by, replay, received_at)
                VALUES (?1, ?2, 1, 1, ?3, unhex(?4), 1)";
    for n in [1, 2] {
        let file = replay_of(&f.bridge, n);
        let values = vec![
            id.clone(),
            replays::crc_name(&file).unwrap(),
            f.a.id().as_str().to_owned(),
            file.iter().map(|b| format!("{b:02x}")).collect(),
        ];
        sql(&f, kept, values).await;
    }
    assert_eq!(
        send_to_set(&f, &f.b, &id, &replay_of(&f.bridge, 1), "country=NL").await,
        StatusCode::OK
    );
    ember_bridge::maintain(f.bridge.state()).await;
    let list = listed_when(&f, "", |list| {
        games(list).len() == 2 && list["sets"][&id]["pack"].is_string()
    })
    .await;
    for n in [1, 2] {
        assert_eq!(
            listed_game(&list, &replay_of(&f.bridge, n))["set"],
            id.as_str()
        );
    }
    assert_eq!(
        listed_game(&list, &replay_of(&f.bridge, 1))["countries"],
        json!([null, "NL"])
    );
    assert_eq!(games(&listed(&f, "event=online").await).len(), 0);
    let left = count(
        &f,
        "SELECT COUNT(*) FROM match_replays WHERE match_id = ?1 AND archived = 0",
        &id,
    )
    .await;
    assert_eq!(left, 0);
}

/// A set's replay kept under the rowid of one retention removed is still
/// one the archive has not seen.
#[tokio::test]
async fn a_replay_under_a_reused_rowid_is_still_backfilled() {
    let f = fixture().await;
    let id = set(&f, "archive-reused", 2).await;
    let kept = "INSERT INTO match_replays (match_id, crc, revision, saved_at, sent_by, replay, received_at)
                VALUES (?1, ?2, 1, 1, ?3, unhex(?4), 1)";
    let values = |file: Vec<u8>| {
        vec![
            id.clone(),
            replays::crc_name(&file).unwrap(),
            f.a.id().as_str().to_owned(),
            file.iter().map(|b| format!("{b:02x}")).collect(),
        ]
    };
    let rowid = "SELECT MAX(rowid) FROM match_replays WHERE match_id = ?1";
    sql(&f, kept, values(replay_of(&f.bridge, 1))).await;
    let first = count(&f, rowid, &id).await;
    ember_bridge::maintain(f.bridge.state()).await;
    listed_when(&f, "", |list| games(list).len() == 1).await;
    sql(
        &f,
        "DELETE FROM match_replays WHERE match_id = ?1",
        vec![id.clone()],
    )
    .await;
    sql(&f, kept, values(replay_of(&f.bridge, 2))).await;
    assert_eq!(count(&f, rowid, &id).await, first, "the rowid is reused");
    ember_bridge::maintain(f.bridge.state()).await;
    let list = listed_when(&f, "", |list| games(list).len() == 2).await;
    assert_eq!(
        listed_game(&list, &replay_of(&f.bridge, 2))["set"],
        id.as_str()
    );
}

/// Replay retention keeps a set's replays until the bucket holds its pack,
/// and a set it pruned takes no late game: its pack stays whole.
#[tokio::test]
async fn a_sets_replays_stay_until_its_pack_is_stored_then_it_closes() {
    let f = fixture_with(|config| {
        config.replay_retention = Some(ember_bridge::config::ReplayRetention {
            days: 30,
            backup: None,
        })
    })
    .await;
    f.refusing.store(true, Ordering::SeqCst);
    let id = set(&f, "archive-retained", 2).await;
    assert_eq!(
        send_to_set(&f, &f.a, &id, &replay_of(&f.bridge, 1), "").await,
        StatusCode::CREATED
    );
    let held = || {
        count(
            &f,
            "SELECT COUNT(*) FROM match_replays WHERE match_id = ?1",
            &id,
        )
    };
    // Unplayed, the set expires after a day, which is its last change.
    f.bridge.clock.advance(DAY + 1);
    ember_bridge::maintain(f.bridge.state()).await;
    f.bridge.clock.advance(31 * DAY);
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(held().await, 1, "the pack is not stored yet");
    f.refusing.store(false, Ordering::SeqCst);
    // Refused for a day and more, the archive tries again hourly.
    pack_stored(&f, &id, 1).await;
    ember_bridge::maintain(f.bridge.state()).await;
    assert_eq!(held().await, 0);
    let packs = || {
        f.stored
            .lock()
            .unwrap()
            .iter()
            .filter(|(path, _)| path.contains("/sets/"))
            .count()
    };
    let stored_packs = packs();
    let version = "SELECT COUNT(*) FROM archive_sets WHERE match_id = ?1
                   AND version = 1 AND uploaded_version = 1 AND pruned_at IS NOT NULL";
    assert_eq!(count(&f, version, &id).await, 1);
    // The set's second game, late, from a player signed in again.
    let mut again = f.bridge.player(41);
    f.bridge.open_session(&mut again).await;
    assert_eq!(again.id(), f.a.id());
    let (status, refused) =
        set_answer(&f, &again, &id, &replay_of(&f.bridge, 2), "revision=1").await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    assert_eq!(
        refused["error"]["details"]["reason"], "set_closed",
        "{refused}"
    );
    assert_eq!(held().await, 0);
    assert_eq!(count(&f, version, &id).await, 1);
    assert_eq!(packs(), stored_packs);
}

/// A replay that arrives while its game waits for its second report is
/// sent again later (503, `replay_unconfirmed`), and kept once the game is
/// confirmed.
#[tokio::test]
async fn a_replay_of_a_game_not_confirmed_yet_is_sent_again_later() {
    let f = fixture().await;
    let id = create_match(
        &f.bridge,
        &f.provider,
        &f.links,
        [&f.a, &f.b],
        "archive-unconfirmed",
        "ember-room-v1",
        2,
    )
    .await;
    let binding = bound(&f, &id).await;
    let game = permit(&f, &id, &binding, 20).await;
    report(&f, &f.a, &f.a, &id, &game, 1, Outcome::Draw).await;
    let (status, later) = set_answer(&f, &f.a, &id, &replay_of(&f.bridge, 1), "revision=1").await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
    assert_eq!(later["error"]["code"], "service_unavailable", "{later}");
    assert_eq!(later["error"]["details"]["reason"], "replay_unconfirmed");
    let (status, body) = report(&f, &f.a, &f.b, &id, &game, 2, Outcome::Draw).await;
    assert_eq!(status, StatusCode::CREATED, "{body}");
    assert_eq!(
        send_to_set(&f, &f.a, &id, &replay_of(&f.bridge, 1), "").await,
        StatusCode::CREATED
    );
}

/// A voided game's replay takes nothing from the game played again in its
/// place.
#[tokio::test]
async fn a_voided_games_replay_leaves_its_replacement_free() {
    let f = fixture().await;
    let id = create_match(
        &f.bridge,
        &f.provider,
        &f.links,
        [&f.a, &f.b],
        "archive-voided",
        "ember-room-v1",
        2,
    )
    .await;
    let binding = bound(&f, &id).await;
    for n in [1u8, 2] {
        let game = permit(&f, &id, &binding, 19 + u64::from(n)).await;
        report(&f, &f.a, &f.a, &id, &game, 2 * n - 1, Outcome::Draw).await;
        report(&f, &f.a, &f.b, &id, &game, 2 * n, Outcome::Draw).await;
        assert_eq!(
            send_to_set(&f, &f.a, &id, &replay_of(&f.bridge, n), "").await,
            StatusCode::CREATED
        );
        if n == 1 {
            sql(
                &f,
                "UPDATE attempts SET state = 'voided' WHERE match_id = ?1",
                vec![id.clone()],
            )
            .await;
        }
    }
}

/// A pack not stored yet keeps its set's replays through retention while
/// the archive is turned off, and is stored once it is back on.
#[tokio::test]
async fn an_unstored_pack_keeps_its_sets_replays_while_the_archive_is_off() {
    let f = fixture_with(|config| {
        config.replay_retention = Some(ember_bridge::config::ReplayRetention {
            days: 30,
            backup: None,
        })
    })
    .await;
    f.refusing.store(true, Ordering::SeqCst);
    let id = set(&f, "archive-paused", 1).await;
    assert_eq!(
        send_to_set(&f, &f.a, &id, &replay_of(&f.bridge, 1), "").await,
        StatusCode::CREATED
    );
    f.bridge.clock.advance(DAY + 1);
    ember_bridge::maintain(f.bridge.state()).await;
    f.bridge.clock.advance(31 * DAY);
    // The same bridge with its archive taken out of its configuration.
    let off = ember_bridge::testing::reconfigured(
        f.bridge.state(),
        |config| config.archive = None,
        Default::default(),
    );
    ember_bridge::maintain(&off).await;
    let held = || {
        count(
            &f,
            "SELECT COUNT(*) FROM match_replays WHERE match_id = ?1",
            &id,
        )
    };
    assert_eq!(held().await, 1);
    let closed = "SELECT COUNT(*) FROM archive_sets WHERE match_id = ?1 AND pruned_at IS NOT NULL";
    assert_eq!(count(&f, closed, &id).await, 0);
    f.refusing.store(false, Ordering::SeqCst);
    pack_stored(&f, &id, 1).await;
    assert_eq!(stored_pack(&f, &id).unwrap().len(), 1);
    ember_bridge::maintain(&off).await;
    assert_eq!(held().await, 0);
}

#[tokio::test]
async fn without_an_archive_nothing_is_taken() {
    let bridge = Bridge::start().await;
    let mut a = bridge.player(43);
    bridge.open_session(&mut a).await;
    let played = replay_of(&bridge, 1);
    let crc = replays::crc_name(&played).unwrap();
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
