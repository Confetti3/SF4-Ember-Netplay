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

/// What the stand-in bucket holds: each PUT's path and body, less what was
/// deleted since.
type Stored = Arc<Mutex<Vec<(String, Vec<u8>)>>>;

/// An S3 stand-in that takes any signed PUT whose content hash is right, and
/// answers signed GETs and DELETEs of what it holds, unless it is
/// `refusing`.
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
                    || !signed
                    || headers
                        .get("x-amz-content-sha256")
                        .and_then(|v| v.to_str().ok())
                        != Some(hash.as_str())
                {
                    return (StatusCode::FORBIDDEN, Vec::new());
                }
                let path = uri.path().to_owned();
                let mut held = keep.lock().unwrap();
                match method {
                    axum::http::Method::PUT => {
                        held.push((path, body.to_vec()));
                        (StatusCode::OK, Vec::new())
                    }
                    axum::http::Method::GET => match held.iter().rev().find(|(at, _)| *at == path) {
                        Some((_, file)) => (StatusCode::OK, file.clone()),
                        None => (StatusCode::NOT_FOUND, Vec::new()),
                    },
                    axum::http::Method::DELETE => {
                        held.retain(|(at, _)| *at != path);
                        (StatusCode::NO_CONTENT, Vec::new())
                    }
                    _ => (StatusCode::FORBIDDEN, Vec::new()),
                }
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
        // For the tests that turn public rooms (and so classes) on.
        ..common::supervisor::secrets()
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
            // The sets these tests play are desk events' (the archive takes
            // no other set's games).
            for connection in &mut config.tenants[0].connections {
                connection.kind = "direct".into();
            }
            config.desk = Some(ember_bridge::config::Desk {
                connection: "mock-a".into(),
                invite_only: false,
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

/// An online game `n` long enough to hold a round: its replay goes past the
/// 0x320-byte header, where what was played starts.
fn played_game(n: u8) -> Vec<u8> {
    replays::made_up(&[n; 0x320 - 0x174 + 64], 1_800_000_000, [20, 34])
}

/// `file` as the other player's PC saved the same game: its own time played
/// in the replay's header, so another digest, and its record's CRC to match.
fn their_copy(file: &[u8]) -> Vec<u8> {
    let mut copy = file.to_vec();
    copy[HEAD + 0x10] ^= 0x5a;
    let crc = replays::crc32(&copy[HEAD..]);
    copy[8 + 5..8 + 9].copy_from_slice(&crc.to_le_bytes());
    copy
}

/// The paths of the games the bucket holds.
fn games_held(f: &Fixture) -> Vec<String> {
    let mut held: Vec<String> = f
        .stored
        .lock()
        .unwrap()
        .iter()
        .map(|(path, _)| path.clone())
        .filter(|path| path.starts_with("/replays/games/"))
        .collect();
    held.sort();
    held.dedup();
    held
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

/// An online game names a side only from that side's own player's copy: a
/// player who shares nothing is never named by their opponent's copy, whether
/// that copy is from a game that sends both names or one that sends its own.
#[tokio::test]
async fn an_opponent_is_named_only_by_their_own_copy() {
    let f = fixture().await;
    // An older game sent both names; only its own side's is taken.
    let old = game(11, [1, 2]);
    assert_eq!(
        send(&f, &f.a, &old, "a=Ann&b=Bob&side=0&country=BE").await,
        StatusCode::CREATED
    );
    // A game that sends only its own side, the other empty.
    let new = game(12, [3, 4]);
    assert_eq!(
        send(&f, &f.b, &new, "a=&b=Bob&side=1").await,
        StatusCode::CREATED
    );
    // A copy that does not say whose it is names no one.
    let anonymous = game(13, [5, 6]);
    assert_eq!(
        send(&f, &f.a, &anonymous, "a=Ann&b=Bob").await,
        StatusCode::CREATED
    );
    // The other side is named when its own player's copy of the game comes,
    // and then only by that copy, once.
    assert_eq!(
        send(&f, &f.b, &saved_elsewhere(&old), "a=&b=Bob&side=1").await,
        StatusCode::OK
    );
    assert_eq!(
        send(&f, &f.b, &saved_elsewhere(&old), "a=&b=Mallory&side=1").await,
        StatusCode::OK
    );
    assert_eq!(
        send(&f, &f.a, &saved_elsewhere(&new), "a=Eve&b=Eve&side=0").await,
        StatusCode::OK
    );
    let list = listed_when(&f, "", |list| games(list).len() == 3).await;
    assert_eq!(listed_game(&list, &old)["names"], json!(["Ann", "Bob"]));
    assert_eq!(listed_game(&list, &new)["names"], json!(["Eve", "Bob"]));
    assert_eq!(listed_game(&list, &anonymous)["names"], json!([null, null]));
    // A game nobody's copy named a side of lists it as unnamed.
    let lone = game(14, [7, 8]);
    assert_eq!(
        send(&f, &f.a, &lone, "a=Ann&b=Bob&side=0").await,
        StatusCode::CREATED
    );
    let list = listed_when(&f, "", |list| games(list).len() == 4).await;
    assert_eq!(listed_game(&list, &lone)["names"], json!(["Ann", null]));
    assert_eq!(games(&listed(&f, "q=Mallory").await).len(), 0);
    // The counts are the whole archive's, and what the filter keeps of it.
    let list = listed(&f, "").await;
    assert_eq!((&list["total"], &list["matching"]), (&json!(4), &json!(4)));
    let ann = listed(&f, "q=Ann").await;
    assert_eq!(games(&ann).len(), 2);
    assert_eq!((&ann["total"], &ann["matching"]), (&json!(4), &json!(2)));
    let none = listed(&f, "q=Mallory").await;
    assert_eq!((&none["total"], &none["matching"]), (&json!(4), &json!(0)));
}

/// A later copy names an empty side only from another account than the
/// game's first sender, never the side the first sender said was theirs, and
/// one account names one side. The word stays attributable.
#[tokio::test]
async fn a_late_name_cannot_come_from_the_first_sender_or_fill_both_sides() {
    let f = fixture().await;
    let mut c = f.bridge.player(44);
    f.bridge.open_session(&mut c).await;
    // The first sender names itself, then resends the game as the other
    // side to name an opponent who shared nothing.
    let one = game(21, [1, 2]);
    assert_eq!(
        send(&f, &f.a, &one, "a=Ann&b=&side=0").await,
        StatusCode::CREATED
    );
    assert_eq!(
        send(&f, &f.a, &saved_elsewhere(&one), "a=&b=Bob&side=1").await,
        StatusCode::OK
    );
    // A first sender with no usable name of its own cannot be named by
    // anyone either: a third account naming the side the first sender
    // claimed adds nothing.
    let two = game(22, [3, 4]);
    assert_eq!(
        send(&f, &f.a, &two, "a=&b=&side=0&country=BE").await,
        StatusCode::CREATED
    );
    assert_eq!(
        send(&f, &c, &saved_elsewhere(&two), "a=Eve&b=&side=0").await,
        StatusCode::OK
    );
    // One account names one side of a game, not both.
    assert_eq!(
        send(&f, &f.b, &saved_elsewhere(&two), "a=&b=Bob&side=1").await,
        StatusCode::OK
    );
    let three = game(23, [5, 6]);
    assert_eq!(
        send(&f, &f.a, &three, "side=2").await,
        StatusCode::CREATED
    );
    assert_eq!(
        send(&f, &f.b, &saved_elsewhere(&three), "a=Bob&b=Bob&side=0").await,
        StatusCode::OK
    );
    assert_eq!(
        send(&f, &f.b, &saved_elsewhere(&three), "a=Bob&b=Bob&side=1").await,
        StatusCode::OK
    );
    let list = listed_when(&f, "", |list| games(list).len() == 3).await;
    assert_eq!(listed_game(&list, &one)["names"], json!(["Ann", null]));
    assert_eq!(listed_game(&list, &two)["names"], json!([null, "Bob"]));
    assert_eq!(listed_game(&list, &three)["names"], json!(["Bob", null]));
    // Who named each side is kept.
    let by = |digest: String| async {
        f.bridge
            .state()
            .db
            .read(move |tx| {
                Ok(tx.query_row(
                    "SELECT COALESCE(named_by_a, ''), COALESCE(named_by_b, '') FROM archive_games WHERE digest = ?1",
                    [digest],
                    |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?)),
                )?)
            })
            .await
            .unwrap()
    };
    assert_eq!(by(digest(&one)).await, (String::new(), String::new()));
    assert_eq!(
        by(digest(&two)).await,
        (String::new(), f.b.id().to_string())
    );
    assert_eq!(
        by(digest(&three)).await,
        (f.b.id().to_string(), String::new())
    );
}

/// Both players' copies of one game are one game: the second copy names its
/// own side and country, and is not kept.
#[tokio::test]
async fn both_players_copies_are_one_game() {
    let f = fixture().await;
    let mine = played_game(31);
    let theirs = their_copy(&mine);
    assert_ne!(digest(&mine), digest(&theirs));
    assert_eq!(
        send(&f, &f.a, &mine, "a=Ann&b=&side=0&country=BE").await,
        StatusCode::CREATED
    );
    assert_eq!(
        send(&f, &f.b, &theirs, "a=&b=Bob&side=1&country=NL").await,
        StatusCode::OK
    );
    let list = listed_when(&f, "", |list| !games(list).is_empty()).await;
    assert_eq!(games(&list).len(), 1);
    let listed = listed_game(&list, &mine);
    assert_eq!(listed["names"], json!(["Ann", "Bob"]));
    assert_eq!(listed["countries"], json!(["BE", "NL"]));
    assert_eq!(list["countries"], json!(["BE", "NL"]));
    assert_eq!(
        games_held(&f),
        vec![format!("/replays/games/{}.emberreplay", digest(&mine))]
    );
}

/// Copies of one game kept before games were known by what was played are
/// folded into the copy kept first, which takes the other's side, and the
/// other copy leaves the bridge and the bucket.
#[tokio::test]
async fn copies_kept_before_are_folded_into_the_first() {
    let f = fixture().await;
    let mine = played_game(32);
    let theirs = their_copy(&mine);
    assert_eq!(
        send(&f, &f.a, &mine, "a=Ann&b=&side=0&country=BE").await,
        StatusCode::CREATED
    );
    listed_when(&f, "", |list| games(list).len() == 1).await;
    // As a bridge before migration 054 kept them: each copy its own game,
    // neither known by what was played, both stored.
    let (first, second, b) = (digest(&mine), digest(&theirs), f.b.id().to_string());
    f.stored.lock().unwrap().push((
        format!("/replays/games/{second}.emberreplay"),
        theirs.clone(),
    ));
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute("UPDATE archive_games SET game = NULL WHERE digest = ?1", [&first])?;
            tx.execute(
                "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, name_b, country_b,
                    sent_side, sent_by, priority, state, next_attempt_at, received_at, stored_at)
                 SELECT ?1, crc, saved_at, size, fighter_a, fighter_b, 'Bob', 'NL', 1, ?2, 1, 'stored', 0,
                    received_at + 1, received_at + 1
                 FROM archive_games WHERE digest = ?3",
                rusqlite::params![second, b, first],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    let list = listed_when(&f, "", |list| {
        games(list).len() == 1 && games(list)[0]["names"] == json!(["Ann", "Bob"])
    })
    .await;
    assert_eq!(listed_game(&list, &mine)["countries"], json!(["BE", "NL"]));
    for _ in 0..200 {
        if count(&f, "SELECT COUNT(*) FROM archive_games WHERE digest = ?1", &digest(&theirs)).await == 0 {
            break;
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    let left = "SELECT COUNT(*) FROM archive_games WHERE digest = ?1";
    assert_eq!(count(&f, left, &digest(&theirs)).await, 0);
    assert_eq!(
        games_held(&f),
        vec![format!("/replays/games/{}.emberreplay", digest(&mine))]
    );
    let known = "SELECT COUNT(*) FROM archive_games WHERE game = ?1";
    assert_eq!(count(&f, known, &Replay::parse(&mine).unwrap().game_hex()).await, 1);
}

/// Only a desk event's set has its games archived: the organizer accepted
/// desk access for those. A partner's set (any other connection) keeps its
/// replays for the set, but the archive takes none, from the set's route or
/// from the backfill.
#[tokio::test]
async fn only_a_desk_sets_games_are_archived() {
    let f = fixture_with(|config| {
        config.desk = Some(ember_bridge::config::Desk {
            connection: "mock-b".into(),
            invite_only: false,
        });
    })
    .await;
    let id = set(&f, "partner-set", 2).await;
    assert_eq!(
        send_to_set(&f, &f.a, &id, &replay_of(&f.bridge, 1), "country=BE").await,
        StatusCode::CREATED
    );
    let kept = "INSERT INTO match_replays (match_id, crc, revision, saved_at, sent_by, replay, received_at)
                VALUES (?1, ?2, 1, 1, ?3, unhex(?4), 1)";
    let file = replay_of(&f.bridge, 2);
    let values = vec![
        id.clone(),
        replays::crc_name(&file).unwrap(),
        f.a.id().as_str().to_owned(),
        file.iter().map(|b| format!("{b:02x}")).collect(),
    ];
    sql(&f, kept, values).await;
    ember_bridge::maintain(f.bridge.state()).await;
    let in_archive = "SELECT COUNT(*) FROM archive_games WHERE match_id = ?1 OR sent_by = ?1";
    assert_eq!(count(&f, in_archive, &id).await, 0);
    assert_eq!(
        count(&f, in_archive, f.a.id().as_str()).await,
        0,
        "no game of the set, even unmatched"
    );
    // The set still holds both replays for itself, and neither waits for the
    // archive (retention would keep it for one).
    let held = "SELECT COUNT(*) FROM match_replays WHERE match_id = ?1 AND archived = 1";
    assert_eq!(count(&f, held, &id).await, 2);
    assert_eq!(games(&listed(&f, "").await).len(), 0);
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
        send(&f, &f.b, &replay_of(&f.bridge, 1), "a=&b=Bob&side=1").await,
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
        json!([null, "Bob"])
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

/// Once the archive keeps 20 GiB of games it takes no more online games,
/// still a set's games: the archive keeps every set.
#[tokio::test]
async fn a_full_archive_takes_only_sets_games() {
    let f = fixture().await;
    let id = set(&f, "archive-kept", 1).await;
    sql(
        &f,
        "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, sent_by, priority,
            state, next_attempt_at, received_at)
         VALUES (?1, '00000000', 1, 21474836480, 1, 2, 'emb_other', 1, 'stored', 0, 1)",
        vec!["e".repeat(64)],
    )
    .await;
    assert_eq!(
        send(&f, &f.a, &game(1, [1, 2]), "side=0").await,
        StatusCode::SERVICE_UNAVAILABLE
    );
    assert_eq!(
        send_to_set(&f, &f.a, &id, &replay_of(&f.bridge, 1), "").await,
        StatusCode::CREATED
    );
    sql(
        &f,
        "DELETE FROM archive_games WHERE digest = ?1",
        vec!["e".repeat(64)],
    )
    .await;
    assert_eq!(
        send(&f, &f.a, &game(1, [1, 2]), "side=0").await,
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

/// Connects `player` to Discord account `account`.
async fn connect(f: &Fixture, player: &Player, account: &str) {
    common::seed::link_discord(f.bridge.state(), account, player.id().as_str(), "player").await;
}

/// Gives Discord account `account` its ranking choice (`listed` under a
/// name, or not) and a rating in the middle of `band`, settled or not,
/// played an hour ago.
async fn ranked(f: &Fixture, account: &str, listed: bool, band: u8, settled: bool) {
    let played_at = f.bridge.state().now() as i64 - 3600;
    let rating = 1050.0 + 100.0 * f64::from(band);
    let deviation = if settled { 60.0 } else { 300.0 };
    let account = account.to_owned();
    f.bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO ranking_choices (discord_user_id, ranked, name, country)
                 VALUES (?1, 1, ?2, NULL)
                 ON CONFLICT (discord_user_id) DO UPDATE SET name = excluded.name",
                rusqlite::params![account, listed.then_some("Listed")],
            )?;
            tx.execute(
                "INSERT INTO skill_ratings (discord_user_id, rating, deviation, volatility, band, played_at)
                 VALUES (?1, ?2, ?3, 0.06, ?4, ?5)
                 ON CONFLICT (discord_user_id) DO UPDATE SET deviation = excluded.deviation",
                rusqlite::params![account, rating, deviation, band, played_at],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

/// An archive bridge with public rooms, and classes shown or not.
async fn with_classes(shown: bool) -> Fixture {
    let (_, url) = common::supervisor::fake_supervisor().await;
    fixture_with(move |config| {
        common::supervisor::enable(config, url);
        let rooms = config.rooms.as_mut().unwrap();
        rooms.skill_badges = shown;
        rooms.developers = vec!["300".to_owned()];
    })
    .await
}

/// Each side lists the class the public rankings board shows today for the
/// player who played it, as far as the archive knows who did: a set's
/// roster, or the account that sent or named that side. A player who is not
/// on the board (not listed, not settled, a developer) shows none.
#[tokio::test]
async fn a_side_lists_its_players_class_as_the_board_shows_it() {
    let f = with_classes(true).await;
    connect(&f, &f.a, "100").await;
    connect(&f, &f.b, "200").await;
    ranked(&f, "100", true, 7, true).await;
    ranked(&f, "200", false, 3, true).await;
    let online = game(21, [1, 2]);
    assert_eq!(
        send(&f, &f.a, &online, "a=Ann&b=Bob&side=0").await,
        StatusCode::CREATED
    );
    // Who sent a copy that says no side is not known to have played either.
    let unsided = game(22, [3, 4]);
    assert_eq!(
        send(&f, &f.a, &unsided, "a=Ann&b=Bob").await,
        StatusCode::CREATED
    );
    let list = listed_when(&f, "", |list| games(list).len() == 2).await;
    assert_eq!(listed_game(&list, &online)["classes"], json!([7, null]));
    assert_eq!(listed_game(&list, &unsided)["classes"], json!([null, null]));
    // The opponent names their side; their class shows once they are listed
    // and settled, and not while they place again.
    assert_eq!(
        send(&f, &f.b, &saved_elsewhere(&online), "a=&b=Bob&side=1").await,
        StatusCode::OK
    );
    let list = listed(&f, "").await;
    assert_eq!(listed_game(&list, &online)["classes"], json!([7, null]));
    ranked(&f, "200", true, 3, true).await;
    let list = listed(&f, "").await;
    assert_eq!(listed_game(&list, &online)["classes"], json!([7, 3]));
    ranked(&f, "200", true, 3, false).await;
    let list = listed(&f, "").await;
    assert_eq!(listed_game(&list, &online)["classes"], json!([7, null]));
    // A set's sides are its roster's, whoever sent the game.
    ranked(&f, "200", true, 3, true).await;
    let id = set(&f, "archive-classes", 1).await;
    assert_eq!(
        send_to_set(&f, &f.a, &id, &replay_of(&f.bridge, 1), "country=BE").await,
        StatusCode::CREATED
    );
    let list = listed_when(&f, "", |list| games(list).len() == 3).await;
    assert_eq!(
        listed_game(&list, &replay_of(&f.bridge, 1))["classes"],
        json!([7, 3])
    );
    // No Ember ID or Discord account is ever listed.
    let text = list.to_string();
    for hidden in [f.a.id().as_str(), f.b.id().as_str(), "\"100\"", "\"200\""] {
        assert!(!text.contains(hidden), "{hidden} in {text}");
    }
}

/// A developer is not on the board, so their side shows no class; nor does
/// any side while the bridge hides classes.
#[tokio::test]
async fn no_class_for_a_developer_or_while_classes_are_hidden() {
    let f = with_classes(true).await;
    connect(&f, &f.a, "300").await;
    ranked(&f, "300", true, 7, true).await;
    let online = game(23, [1, 2]);
    assert_eq!(
        send(&f, &f.a, &online, "a=Ann&b=&side=0").await,
        StatusCode::CREATED
    );
    let list = listed_when(&f, "", |list| games(list).len() == 1).await;
    assert_eq!(listed_game(&list, &online)["classes"], json!([null, null]));

    let f = with_classes(false).await;
    connect(&f, &f.a, "100").await;
    ranked(&f, "100", true, 7, true).await;
    assert_eq!(
        send(&f, &f.a, &online, "a=Ann&b=&side=0").await,
        StatusCode::CREATED
    );
    let list = listed_when(&f, "", |list| games(list).len() == 1).await;
    assert_eq!(listed_game(&list, &online)["classes"], json!([null, null]));
}
