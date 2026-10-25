//! The replay channel: uploads of room games, the choice to share, the list
//! for players and the website, and the files. Names come from each player's
//! own upload, and a player who stops sharing is off every page at once.
mod common;

use common::{
    Bridge, Player,
    public_rooms::{open_room, player, send},
    sites::connect_discord,
    supervisor::{Supervisor, enable, fake_supervisor, report, secrets},
};
use ember_protocol::{
    encoding::b64,
    replays::{EXPORT_MAGIC, RECORD_BYTES, ReplayUpload, crc32},
};
use reqwest::{Method, StatusCode};
use serde_json::{Value as Json_, json};

const REPLAYS: &str = "/v1/replays";
const ME: &str = "/v1/replays/me";
const PUBLIC: &str = "/v1/replays/public";
const DAY: i64 = 24 * 60 * 60;
/// Fighters of the usual test replay.
const FIGHTERS: [u8; 2] = [39, 23];

async fn start(show: bool, developers: &[&str]) -> (Bridge, Supervisor) {
    let (fake, url) = fake_supervisor().await;
    let developers: Vec<String> = developers.iter().map(|id| (*id).to_owned()).collect();
    let bridge = Bridge::start_with(
        |config| {
            enable(config, url);
            let rooms = config.rooms.as_mut().unwrap();
            rooms.skill_badges = show;
            rooms.developers = developers;
        },
        secrets(),
    )
    .await;
    // Games are timed from the clock, so it stands still.
    bridge.clock.freeze();
    (bridge, fake)
}

/// Two players with Discord accounts "100" and "200".
async fn two(bridge: &Bridge) -> (Player, Player) {
    let (first, second) = (player(bridge, 1).await, player(bridge, 2).await);
    connect_discord(bridge, "100", &first).await;
    connect_discord(bridge, "200", &second).await;
    (first, second)
}

/// An `.emberreplay` of `fighters` played at `time`; `tag` tells replays apart.
fn export(time: u64, fighters: [u8; 2], tag: u8) -> Vec<u8> {
    let mut replay = vec![0u8; 0x400];
    replay[..4].copy_from_slice(b"#BRP");
    let filetime = time * 10_000_000 + 116_444_736_000_000_000;
    replay[0x10..0x18].copy_from_slice(&filetime.to_le_bytes());
    replay[0x20..0x24].copy_from_slice(&u32::from(fighters[0]).to_le_bytes());
    replay[0x170..0x174].copy_from_slice(&u32::from(fighters[1]).to_le_bytes());
    replay[0x200] = tag;
    let mut record = vec![0u8; RECORD_BYTES];
    record[4] = 1;
    record[5..9].copy_from_slice(&crc32(&replay).to_le_bytes());
    record[9..13].copy_from_slice(&(replay.len() as u32).to_le_bytes());
    record[22..26].copy_from_slice(&16u32.to_le_bytes());
    record[26..42].copy_from_slice(b"2026-10-25 12:00");
    record[71] = fighters[0];
    record[105] = fighters[1];
    let mut out = EXPORT_MAGIC.to_vec();
    out.extend_from_slice(&record);
    out.extend_from_slice(&[0x0E, 0x0E]);
    out.extend_from_slice(&replay);
    out
}

/// A game's key in an upload: room, table 0 and generation.
struct Game<'a> {
    room_id: &'a str,
    generation: u64,
    played_at: u64,
    fighters: [u8; 2],
}

impl<'a> Game<'a> {
    fn new(room_id: &'a str, generation: u64, played_at: u64) -> Self {
        Self {
            room_id,
            generation,
            played_at,
            fighters: FIGHTERS,
        }
    }

    fn body(&self, side: u8, name: &str, country: Option<&str>) -> Json_ {
        let bytes = export(self.played_at, self.fighters, self.generation as u8);
        let upload = ReplayUpload::new(
            self.room_id.to_owned(),
            0,
            self.generation,
            side,
            name.to_owned(),
            country.map(str::to_owned),
            &bytes,
        );
        serde_json::to_value(upload).unwrap()
    }
}

async fn call(
    bridge: &Bridge,
    player: &Player,
    method: Method,
    path: &str,
    body: Option<Json_>,
) -> (StatusCode, Json_) {
    send(bridge, method, player.token(), "198.51.100.7", path, body).await
}

async fn upload(bridge: &Bridge, player: &Player, body: Json_) -> (StatusCode, Json_) {
    call(bridge, player, Method::POST, REPLAYS, Some(body)).await
}

/// Uploads and expects the bridge to take it.
async fn uploaded(bridge: &Bridge, player: &Player, body: Json_) -> Json_ {
    let (status, answer) = upload(bridge, player, body).await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    answer
}

async fn page(bridge: &Bridge, player: &Player, query: &str) -> Json_ {
    let path = format!("{REPLAYS}{query}");
    let (status, page) = call(bridge, player, Method::GET, &path, None).await;
    assert_eq!(status, StatusCode::OK, "{page}");
    page
}

/// The page the website reads, with no credential.
async fn public(bridge: &Bridge, query: &str) -> Json_ {
    let request = bridge.client.get(bridge.url(&format!("{PUBLIC}{query}")));
    let (status, page) = common::read(request.send().await.unwrap()).await;
    assert_eq!(status, StatusCode::OK, "{page}");
    page
}

fn listed(page: &Json_) -> Vec<String> {
    page["replays"]
        .as_array()
        .unwrap()
        .iter()
        .map(|entry| entry["id"].as_str().unwrap().to_owned())
        .collect()
}

async fn sharing(bridge: &Bridge, player: &Player, on: bool) -> (StatusCode, Json_) {
    call(
        bridge,
        player,
        Method::PUT,
        ME,
        Some(json!({ "sharing": on })),
    )
    .await
}

/// What the supervisor reports as the room's recent games: (generation, winner, loser).
fn report_games(fake: &Supervisor, room_id: &str, games: &[(u64, &Player, &Player)]) {
    let mut fake = fake.lock().unwrap();
    let room = fake
        .rooms
        .iter_mut()
        .find(|room| room["room_id"] == room_id)
        .expect("the supervisor hosts the room");
    room["games"] = Json_::Array(
        games
            .iter()
            .map(|(generation, winner, loser)| {
                json!({ "table": 0, "generation": generation,
                    "winner": winner.id().as_str(), "loser": loser.id().as_str() })
            })
            .collect(),
    );
}

/// A public room with two members and the games its host reports.
async fn played(
    bridge: &Bridge,
    fake: &Supervisor,
    creator: &Player,
    games: &[(u64, &Player, &Player)],
) -> String {
    let room_id = open_room(bridge, creator, "198.51.100.1", 8).await;
    report(fake, &room_id, 2, 1, &[]);
    report_games(fake, &room_id, games);
    ember_bridge::poll_rooms(bridge.state()).await;
    room_id
}

/// A room id no room has: a private room's, which the bridge never hears of.
fn private_room() -> String {
    "5".repeat(32)
}

fn now(bridge: &Bridge) -> u64 {
    bridge.state().now()
}

/// Gives Discord account `account` a settled rating in the middle of `band`.
async fn rate(bridge: &Bridge, account: &str, band: u8) {
    let (account, played_at) = (account.to_owned(), now(bridge));
    let rating = 1050.0 + 100.0 * f64::from(band);
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO skill_ratings
                     (discord_user_id, rating, deviation, volatility, band, played_at)
                 VALUES (?1, ?2, 60.0, 0.06, ?3, ?4)",
                rusqlite::params![account, rating, band, played_at],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

async fn count(bridge: &Bridge, table: &'static str) -> i64 {
    bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(&format!("SELECT COUNT(*) FROM {table}"), [], |r| r.get(0))?)
        })
        .await
        .unwrap()
}

#[tokio::test]
async fn a_public_game_is_listed_when_a_player_uploads_it_after_the_host_report() {
    let (bridge, fake) = start(false, &[]).await;
    let (first, second) = two(&bridge).await;
    let room_id = played(&bridge, &fake, &first, &[(1, &second, &first)]).await;
    let game = Game::new(&room_id, 1, now(&bridge) - 60);

    let answer = uploaded(&bridge, &first, game.body(1, "Player 1", Some("GB"))).await;
    assert_eq!(answer["state"], "published");
    let id = answer["id"].as_str().unwrap().to_owned();

    let mine = page(&bridge, &first, "").await;
    assert_eq!(listed(&mine), vec![id.clone()]);
    let entry = &mine["replays"][0];
    assert_eq!(entry["room"], "public");
    // The second player won, and played the other slot.
    assert_eq!(entry["winner"], 0);
    assert_eq!(entry["played_at"], now(&bridge) - 60);
    assert_eq!(
        entry["sides"][1],
        json!({ "name": "Player 1", "country": "GB", "fighter": 23, "own": true })
    );
    // The side that never uploaded has no listing yet: a default name.
    assert_eq!(
        entry["sides"][0],
        json!({ "name": "Player 1", "fighter": 39 })
    );
    // The opponent sees the same game with their own side marked.
    let theirs = page(&bridge, &second, "").await;
    assert_eq!(theirs["replays"][0]["sides"][0]["own"], true);
    assert!(theirs["replays"][0]["sides"][1].get("own").is_none());
    // The website's page marks no one.
    let site = public(&bridge, "").await;
    assert_eq!(listed(&site), vec![id.clone()]);
    assert!(site["replays"][0]["sides"][0].get("own").is_none());
    assert!(site["replays"][0]["sides"][1].get("own").is_none());

    // The same player again is the same entry.
    let again = uploaded(&bridge, &first, game.body(1, "Player 1", Some("GB"))).await;
    assert_eq!(again, answer);
    assert_eq!(count(&bridge, "channel_replays").await, 1);
}

#[tokio::test]
async fn an_upload_before_the_host_report_is_sent_again_after_it() {
    let (bridge, fake) = start(false, &[]).await;
    let (first, second) = two(&bridge).await;
    let room_id = open_room(&bridge, &first, "198.51.100.1", 8).await;
    report(&fake, &room_id, 2, 1, &[]);
    let game = Game::new(&room_id, 1, now(&bridge) - 60);

    // Nothing is kept for a public room's game before its report, so nobody
    // can bind an upload to it first.
    let (status, refused) = upload(&bridge, &first, game.body(0, "Player 1", None)).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE, "{refused}");
    assert_eq!(refused["error"]["details"]["reason"], "replay_not_reported");
    assert_eq!(count(&bridge, "channel_replays").await, 0);
    assert!(listed(&public(&bridge, "").await).is_empty());

    report_games(&fake, &room_id, &[(1, &first, &second)]);
    ember_bridge::poll_rooms(bridge.state()).await;
    let answer = uploaded(&bridge, &first, game.body(0, "Player 1", None)).await;
    assert_eq!(answer["state"], "published");
    let after = page(&bridge, &first, "").await;
    assert_eq!(after["replays"].as_array().unwrap().len(), 1);
    let entry = &after["replays"][0];
    assert_eq!(entry["room"], "public");
    assert_eq!(entry["winner"], 0);
    assert_eq!(entry["sides"][0]["name"], "Player 1");
    assert_eq!(entry["id"], answer["id"]);
}

#[tokio::test]
async fn an_upload_for_a_game_the_host_reported_without_the_player_is_refused() {
    let (bridge, fake) = start(false, &[]).await;
    let (first, second) = two(&bridge).await;
    let third = player(&bridge, 3).await;
    connect_discord(&bridge, "300", &third).await;
    let room_id = played(&bridge, &fake, &first, &[(1, &first, &second)]).await;
    let game = Game::new(&room_id, 1, now(&bridge) - 60);

    let (status, refused) = upload(&bridge, &third, game.body(0, "Player 3", None)).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{refused}");
    assert_eq!(
        refused["error"]["details"]["reason"],
        "replay_not_your_game"
    );
    assert_eq!(count(&bridge, "channel_replays").await, 0);

    // Before the room reports a game, an outsider's upload of it is not kept,
    // and once it does, it is refused as above.
    let next = Game::new(&room_id, 2, now(&bridge) - 50);
    let (status, _) = upload(&bridge, &third, next.body(0, "Player 3", None)).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
    report_games(
        &fake,
        &room_id,
        &[(1, &first, &second), (2, &first, &second)],
    );
    ember_bridge::poll_rooms(bridge.state()).await;
    let (status, _) = upload(&bridge, &third, next.body(0, "Player 3", None)).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    assert_eq!(count(&bridge, "channel_replays").await, 0);
}

#[tokio::test]
async fn a_private_game_is_listed_once_both_players_upload_it() {
    let (bridge, _fake) = start(false, &[]).await;
    let (first, second) = two(&bridge).await;
    let third = player(&bridge, 3).await;
    connect_discord(&bridge, "300", &third).await;
    let room_id = private_room();
    let game = Game::new(&room_id, 4, now(&bridge) - 60);

    let held = uploaded(&bridge, &first, game.body(0, "Player 1", Some("GB"))).await;
    assert_eq!(held, json!({ "state": "held" }));
    // The same player again changes nothing.
    assert_eq!(
        uploaded(&bridge, &first, game.body(0, "Player 1", Some("GB"))).await,
        held
    );
    assert!(listed(&public(&bridge, "").await).is_empty());

    // Anything that is not the other player's upload of the same game is refused.
    let other_fighters = Game {
        fighters: [1, 2],
        ..Game::new(&room_id, 4, now(&bridge) - 60)
    };
    for (who, body) in [
        (&third, game.body(0, "Player 3", None)),
        (&first, game.body(1, "Player 1", None)),
        (&second, other_fighters.body(1, "Player 2", None)),
    ] {
        let (status, refused) = upload(&bridge, who, body).await;
        assert_eq!(status, StatusCode::FORBIDDEN, "{refused}");
        assert_eq!(
            refused["error"]["details"]["reason"],
            "replay_not_your_game"
        );
    }

    let answer = uploaded(&bridge, &second, game.body(1, "Player 2", Some("FR"))).await;
    assert_eq!(answer["state"], "published");
    // The website page made before is kept for 15 s.
    bridge.clock.advance(15);
    let site = public(&bridge, "").await;
    assert_eq!(
        listed(&site),
        vec![answer["id"].as_str().unwrap().to_owned()]
    );
    let entry = &site["replays"][0];
    assert_eq!(entry["room"], "private");
    assert!(entry.get("winner").is_none());
    assert_eq!(entry["sides"][0]["name"], "Player 1");
    assert_eq!(entry["sides"][0]["country"], "GB");
    assert_eq!(entry["sides"][1]["name"], "Player 2");
    assert_eq!(entry["sides"][1]["country"], "FR");

    // Nobody else can add to a listed game.
    let (status, _) = upload(&bridge, &third, game.body(1, "Player 3", None)).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
}

#[tokio::test]
async fn names_are_never_taken_from_the_opponents_upload() {
    let (bridge, fake) = start(false, &[]).await;
    let (first, second) = two(&bridge).await;
    let (status, chosen) = call(
        &bridge,
        &second,
        Method::PUT,
        "/v1/ranking/me",
        Some(json!({ "ranked": true, "listing": { "name": "Player 3", "country": "JP" } })),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{chosen}");
    let room_id = played(&bridge, &fake, &first, &[(1, &first, &second)]).await;
    let game = Game::new(&room_id, 1, now(&bridge) - 60);

    uploaded(&bridge, &first, game.body(0, "Player 1", Some("GB"))).await;
    let entry = page(&bridge, &first, "").await["replays"][0].clone();
    assert_eq!(entry["sides"][0]["name"], "Player 1");
    // The side that has not uploaded shows how the board lists it.
    assert_eq!(entry["sides"][1]["name"], "Player 3");
    assert_eq!(entry["sides"][1]["country"], "JP");

    // Their own upload names them.
    uploaded(&bridge, &second, game.body(1, "Player 2", Some("FR"))).await;
    let entry = page(&bridge, &first, "").await["replays"][0].clone();
    assert_eq!(entry["sides"][0]["name"], "Player 1");
    assert_eq!(entry["sides"][1]["name"], "Player 2");
    assert_eq!(entry["sides"][1]["country"], "FR");
    assert_eq!(count(&bridge, "channel_replays").await, 1);
}

#[tokio::test]
async fn turning_sharing_off_hides_a_players_replays_at_once() {
    let (bridge, fake) = start(false, &[]).await;
    let (first, second) = two(&bridge).await;
    let room_id = played(
        &bridge,
        &fake,
        &first,
        &[(1, &first, &second), (2, &first, &second)],
    )
    .await;
    let early = Game::new(&room_id, 1, now(&bridge) - 60);
    let id = uploaded(&bridge, &first, early.body(0, "Player 1", None)).await["id"]
        .as_str()
        .unwrap()
        .to_owned();
    // The website page is made once and kept.
    assert_eq!(listed(&public(&bridge, "").await), vec![id.clone()]);
    let file = format!("/v1/replays/{id}/file");
    assert_eq!(
        bridge
            .client
            .get(bridge.url(&file))
            .send()
            .await
            .unwrap()
            .status(),
        StatusCode::OK
    );

    // The other player stops sharing: the entry is gone from every page and
    // its file, with no wait.
    let (status, off) = sharing(&bridge, &second, false).await;
    assert_eq!(status, StatusCode::OK, "{off}");
    assert_eq!(off, json!({ "sharing": "off" }));
    for who in [&first, &second] {
        assert!(listed(&page(&bridge, who, "").await).is_empty());
    }
    assert!(listed(&public(&bridge, "").await).is_empty());
    assert_eq!(
        bridge
            .client
            .get(bridge.url(&file))
            .send()
            .await
            .unwrap()
            .status(),
        StatusCode::NOT_FOUND
    );

    // A new upload of theirs, or against them, is kept but not shared.
    let later = Game::new(&room_id, 2, now(&bridge) - 30);
    let answer = uploaded(&bridge, &first, later.body(0, "Player 1", None)).await;
    assert_eq!(answer, json!({ "state": "not_shared" }));
    let (_, mine) = call(&bridge, &second, Method::GET, ME, None).await;
    assert_eq!(mine, json!({ "sharing": "off" }));

    // Turning it back on lists both again.
    let (_, on) = sharing(&bridge, &second, true).await;
    assert_eq!(on, json!({ "sharing": "on" }));
    assert_eq!(listed(&public(&bridge, "").await).len(), 2);
    assert_eq!(
        bridge
            .client
            .get(bridge.url(&file))
            .send()
            .await
            .unwrap()
            .status(),
        StatusCode::OK
    );
}

#[tokio::test]
async fn a_player_without_a_discord_account_shares_nothing() {
    let (bridge, fake) = start(false, &[]).await;
    let (first, second) = two(&bridge).await;
    let loner = player(&bridge, 3).await;
    let room_id = played(&bridge, &fake, &first, &[(1, &first, &second)]).await;
    let game = Game::new(&room_id, 1, now(&bridge) - 60);

    let answer = uploaded(&bridge, &loner, game.body(0, "Player 3", None)).await;
    assert_eq!(answer, json!({ "state": "not_shared" }));
    assert_eq!(count(&bridge, "channel_replays").await, 0);

    let (status, mine) = call(&bridge, &loner, Method::GET, ME, None).await;
    assert_eq!(
        (status, mine),
        (StatusCode::OK, json!({ "sharing": "no_account" }))
    );
    let (status, refused) = sharing(&bridge, &loner, false).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{refused}");
    assert_eq!(refused["error"]["details"]["reason"], "no_discord_account");
    let (_, mine) = call(&bridge, &first, Method::GET, ME, None).await;
    assert_eq!(mine, json!({ "sharing": "on" }));
}

#[tokio::test]
async fn pages_are_newest_first_and_filter_by_fighter_class_and_name() {
    let (bridge, fake) = start(true, &[]).await;
    let (first, second) = two(&bridge).await;
    rate(&bridge, "100", 5).await;
    // The second player does not take part in ranking, so shows no class.
    let (status, out) = call(
        &bridge,
        &second,
        Method::PUT,
        "/v1/ranking/me",
        Some(json!({ "ranked": false })),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{out}");
    let games = [
        (1, &first, &second),
        (2, &first, &second),
        (3, &first, &second),
    ];
    let room_id = played(&bridge, &fake, &first, &games).await;
    let base = now(&bridge) - 1000;
    let mut ids = Vec::new();
    for (generation, fighters, name) in [
        (1u64, [1u8, 2u8], "Player 1"),
        (2, [3, 4], "Player 12"),
        (3, [1, 4], "Player 1"),
    ] {
        let game = Game {
            fighters,
            ..Game::new(&room_id, generation, base + generation)
        };
        let answer = uploaded(&bridge, &first, game.body(0, name, None)).await;
        ids.push(answer["id"].as_str().unwrap().to_owned());
    }

    let all = page(&bridge, &first, "").await;
    assert_eq!(
        listed(&all),
        vec![ids[2].clone(), ids[1].clone(), ids[0].clone()]
    );
    assert!(all.get("more").is_none());
    // Classes are the ranking's at the time: the second player shows none.
    assert_eq!(
        all["replays"][0]["sides"][0]["badge"],
        json!({ "band": 5, "settled": true })
    );
    assert!(all["replays"][0]["sides"][1].get("badge").is_none());

    // A fighter on either side.
    assert_eq!(
        listed(&page(&bridge, &first, "?fighter=4").await),
        vec![ids[2].clone(), ids[1].clone()]
    );
    assert_eq!(
        listed(&public(&bridge, "?fighter=1").await),
        vec![ids[2].clone(), ids[0].clone()]
    );
    assert!(listed(&public(&bridge, "?fighter=9").await).is_empty());
    // A class either side held.
    assert_eq!(listed(&public(&bridge, "?band=5").await).len(), 3);
    assert!(listed(&public(&bridge, "?band=4").await).is_empty());
    // Part of a name, without case.
    assert_eq!(
        listed(&public(&bridge, "?name=LAYER%2012").await),
        vec![ids[1].clone()]
    );
    assert_eq!(listed(&public(&bridge, "?name=player").await).len(), 3);
    // Filters together.
    assert_eq!(
        listed(&page(&bridge, &first, "?fighter=1&name=player%201").await),
        vec![ids[2].clone(), ids[0].clone()]
    );
    assert!(listed(&page(&bridge, &first, "?fighter=2&name=player%2012").await).is_empty());
    // A filter that is not one is refused.
    for bad in [
        "?fighter=64",
        "?band=11",
        "?band=0",
        "?before=nope",
        "?name=",
    ] {
        let (status, refused) = call(
            &bridge,
            &first,
            Method::GET,
            &format!("{REPLAYS}{bad}"),
            None,
        )
        .await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{bad} {refused}");
    }
}

#[tokio::test]
async fn a_page_holds_fifty_and_the_cursor_goes_on_from_its_last_entry() {
    let (bridge, fake) = start(false, &[]).await;
    let (first, second) = two(&bridge).await;
    let games: Vec<(u64, &Player, &Player)> = (1..=55)
        .map(|generation| (generation, &first, &second))
        .collect();
    let room_id = played(&bridge, &fake, &first, &games).await;
    let base = now(&bridge) - 1000;
    let mut ids = Vec::new();
    for generation in 1..=55u64 {
        let game = Game::new(&room_id, generation, base + generation);
        let answer = uploaded(&bridge, &first, game.body(0, "Player 1", None)).await;
        ids.push(answer["id"].as_str().unwrap().to_owned());
    }
    // Newest first: generation 55 down to 6, then 5 down to 1.
    let newest: Vec<String> = ids.iter().rev().cloned().collect();

    let one = public(&bridge, "").await;
    assert_eq!(listed(&one), newest[..50]);
    assert_eq!(one["more"], true);
    let rest = public(&bridge, &format!("?before={}", newest[49])).await;
    assert_eq!(listed(&rest), newest[50..]);
    assert!(rest.get("more").is_none());
    // The player's page pages the same way.
    let mine = page(&bridge, &first, &format!("?before={}", newest[49])).await;
    assert_eq!(listed(&mine), newest[50..]);
    // An entry that is not there ends the paging.
    let none = public(&bridge, &format!("?before={}", "a".repeat(32))).await;
    assert!(listed(&none).is_empty());
}

#[tokio::test]
async fn the_file_is_the_export_for_a_listed_replay_and_nothing_else() {
    let (bridge, fake) = start(false, &[]).await;
    let (first, second) = two(&bridge).await;
    let room_id = played(&bridge, &fake, &first, &[(1, &first, &second)]).await;
    let game = Game::new(&room_id, 1, now(&bridge) - 60);
    let body = game.body(0, "Player 1", None);
    let id = uploaded(&bridge, &first, body.clone()).await["id"]
        .as_str()
        .unwrap()
        .to_owned();

    let response = bridge
        .client
        .get(bridge.url(&format!("/v1/replays/{id}/file")))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let header = |name: &str| response.headers()[name].to_str().unwrap().to_owned();
    assert_eq!(header("content-type"), "application/octet-stream");
    assert_eq!(
        header("content-disposition"),
        format!("attachment; filename=\"{id}.emberreplay\"")
    );
    assert_eq!(header("cache-control"), "public, max-age=300");
    let bytes = response.bytes().await.unwrap();
    assert_eq!(
        &bytes[..],
        export(now(&bridge) - 60, FIGHTERS, 1).as_slice()
    );
    assert_eq!(body["replay"], b64(&bytes));

    for unknown in ["f".repeat(32), "nope".to_owned()] {
        let response = bridge
            .client
            .get(bridge.url(&format!("/v1/replays/{unknown}/file")))
            .send()
            .await
            .unwrap();
        let (status, refused) = common::read(response).await;
        assert_eq!(status, StatusCode::NOT_FOUND, "{refused}");
        assert_eq!(refused["error"]["details"]["reason"], "replay_not_found");
    }
    // A held replay has no file to fetch.
    let private = private_room();
    let held = Game::new(&private, 2, now(&bridge) - 60);
    uploaded(&bridge, &first, held.body(0, "Player 1", None)).await;
    let held_id: String = bridge
        .state()
        .db
        .read(|tx| {
            Ok(tx.query_row(
                "SELECT id FROM channel_replays WHERE published = 0",
                [],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap();
    let response = bridge
        .client
        .get(bridge.url(&format!("/v1/replays/{held_id}/file")))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), StatusCode::NOT_FOUND);
    // One file for each of the two replays.
    assert_eq!(
        std::fs::read_dir(bridge_folder(&bridge)).unwrap().count(),
        2
    );
}

/// Where the bridge keeps the replay files.
fn bridge_folder(bridge: &Bridge) -> std::path::PathBuf {
    bridge
        .state()
        .config
        .database
        .parent()
        .unwrap()
        .join("replays")
}

#[tokio::test]
async fn a_developer_shows_the_mark_and_hidden_classes_show_none() {
    let (bridge, fake) = start(true, &["100"]).await;
    let (first, second) = two(&bridge).await;
    rate(&bridge, "200", 7).await;
    let room_id = played(&bridge, &fake, &first, &[(1, &first, &second)]).await;
    let game = Game::new(&room_id, 1, now(&bridge) - 60);
    uploaded(&bridge, &first, game.body(0, "Player 1", None)).await;
    let entry = public(&bridge, "").await["replays"][0].clone();
    assert_eq!(
        entry["sides"][0]["badge"],
        json!({ "band": 0, "settled": false, "developer": true })
    );
    assert_eq!(
        entry["sides"][1]["badge"],
        json!({ "band": 7, "settled": true })
    );
    // The mark is no class to filter by.
    assert_eq!(listed(&public(&bridge, "?band=7").await).len(), 1);
    assert!(listed(&public(&bridge, "?band=5").await).is_empty());

    let (hidden, fake) = start(false, &[]).await;
    let (one, two_) = two(&hidden).await;
    rate(&hidden, "100", 5).await;
    let room_id = played(&hidden, &fake, &one, &[(1, &one, &two_)]).await;
    let game = Game::new(&room_id, 1, now(&hidden) - 60);
    uploaded(&hidden, &one, game.body(0, "Player 1", None)).await;
    let entry = public(&hidden, "").await["replays"][0].clone();
    assert!(entry["sides"][0].get("badge").is_none());
}

#[tokio::test]
async fn an_invalid_replay_is_refused_and_an_account_uploads_a_limited_number_a_day() {
    let (bridge, fake) = start(false, &[]).await;
    let (mut first, second) = two(&bridge).await;
    let room_id = played(&bridge, &fake, &first, &[(1, &first, &second)]).await;
    let game = Game::new(&room_id, 1, now(&bridge) - 60);
    let mut garbled = game.body(0, "Player 1", None);
    garbled["replay"] = json!("AAAA");
    let (status, refused) = upload(&bridge, &first, garbled.clone()).await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{refused}");
    assert_eq!(refused["error"]["details"]["reason"], "replay_invalid");
    let mut bad_name = game.body(0, " Player 1", None);
    bad_name["country"] = json!("gb");
    let (status, refused) = upload(&bridge, &first, bad_name).await;
    assert_eq!(status, StatusCode::BAD_REQUEST, "{refused}");
    assert!(refused["error"]["details"].get("reason").is_none());

    // 2 refusals are spent; the rest of the day's 300 follow.
    for _ in 0..298 {
        let (status, _) = upload(&bridge, &first, garbled.clone()).await;
        assert_eq!(status, StatusCode::BAD_REQUEST);
    }
    let (status, limited) = upload(&bridge, &first, garbled).await;
    assert_eq!(status, StatusCode::TOO_MANY_REQUESTS, "{limited}");
    assert_eq!(limited["error"]["details"]["reason"], "replay_upload_limit");
    // Another account is not held back, and a day later this one is not.
    let ok = uploaded(&bridge, &second, game.body(1, "Player 2", None)).await;
    assert_eq!(ok["state"], "published");
    bridge.clock.advance(DAY);

    bridge.open_session(&mut first).await;
    let ok = uploaded(&bridge, &first, game.body(0, "Player 1", None)).await;
    assert_eq!(ok["state"], "published");
}

#[tokio::test]
async fn held_uploads_and_reported_games_are_kept_for_a_limited_time() {
    let (bridge, fake) = start(false, &[]).await;
    let (first, second) = two(&bridge).await;
    let room_id = played(&bridge, &fake, &first, &[(1, &first, &second)]).await;
    assert_eq!(count(&bridge, "channel_games").await, 1);
    // A game reported again is not kept twice.
    ember_bridge::poll_rooms(bridge.state()).await;
    assert_eq!(count(&bridge, "channel_games").await, 1);
    let private = private_room();
    let held = Game::new(&private, 1, now(&bridge) - 60);
    uploaded(&bridge, &first, held.body(0, "Player 1", None)).await;
    let listed_game = Game::new(&room_id, 1, now(&bridge) - 60);
    uploaded(&bridge, &first, listed_game.body(0, "Player 1", None)).await;
    assert_eq!(count(&bridge, "channel_replays").await, 2);

    // After two days the held upload is gone, the listed replay is not.
    bridge.clock.advance(2 * DAY + 1);
    ember_bridge::maintain(bridge.state()).await;
    assert_eq!(count(&bridge, "channel_replays").await, 1);
    assert_eq!(count(&bridge, "channel_games").await, 1);
    // After two weeks the reported game is forgotten.
    bridge.clock.advance(13 * DAY);
    ember_bridge::maintain(bridge.state()).await;
    assert_eq!(count(&bridge, "channel_games").await, 0);
    assert_eq!(count(&bridge, "channel_replays").await, 1);
}

#[tokio::test]
async fn the_routes_are_not_there_without_public_rooms() {
    let bridge = Bridge::start().await;
    let player = player(&bridge, 1).await;
    let (status, _) = call(&bridge, &player, Method::GET, REPLAYS, None).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = call(&bridge, &player, Method::GET, ME, None).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = upload(&bridge, &player, json!({})).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    for path in [
        PUBLIC.to_owned(),
        format!("/v1/replays/{}/file", "a".repeat(32)),
    ] {
        let response = bridge.client.get(bridge.url(&path)).send().await.unwrap();
        assert_eq!(response.status(), StatusCode::NOT_FOUND, "{path}");
    }
    let features = features(&bridge).await;
    assert!(!features.contains(&json!("replay_channel")));
}

#[tokio::test]
async fn the_channel_is_offered_where_public_rooms_are() {
    let (bridge, _fake) = start(false, &[]).await;
    let features = features(&bridge).await;
    assert!(features.contains(&json!("replay_channel")));
}

/// The features the bridge offers.
async fn features(bridge: &Bridge) -> Vec<Json_> {
    let request = bridge.client.get(bridge.url("/v1/capabilities"));
    let (status, capabilities) = common::read(request.send().await.unwrap()).await;
    assert_eq!(status, StatusCode::OK, "{capabilities}");
    capabilities["features"].as_array().unwrap().clone()
}
