//! What the replay channel tests share: the bridge with public rooms, two
//! players, a replay export, and the requests the tests make.
use crate::common;
pub use crate::common::{
    Bridge, Player,
    public_rooms::{open_room, player, send},
    sites::connect_discord,
    supervisor::{Supervisor, enable, fake_supervisor, report, secrets},
};
pub use ember_protocol::{
    encoding::b64,
    replays::{EXPORT_MAGIC, RECORD_BYTES, ReplayUpload, archive_name, crc32},
};
pub use reqwest::{Method, StatusCode};
pub use serde_json::{Value as Json_, json};

pub const REPLAYS: &str = "/v1/replays";
pub const ME: &str = "/v1/replays/me";
pub const PUBLIC: &str = "/v1/replays/public";
pub const DAY: i64 = 24 * 60 * 60;
/// Fighters of the usual test replay.
pub const FIGHTERS: [u8; 2] = [39, 23];

pub async fn start(show: bool, developers: &[&str]) -> (Bridge, Supervisor) {
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
pub async fn two(bridge: &Bridge) -> (Player, Player) {
    let (first, second) = (player(bridge, 1).await, player(bridge, 2).await);
    connect_discord(bridge, "100", &first).await;
    connect_discord(bridge, "200", &second).await;
    (first, second)
}

/// An `.emberreplay` of `fighters` played at `time`; `tag` tells replays apart.
pub fn export(time: u64, fighters: [u8; 2], tag: u8) -> Vec<u8> {
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
    // Played 2025-10-25 12:00, as the game's records carry it.
    record[51..53].copy_from_slice(&2025u16.to_le_bytes());
    (record[53], record[54], record[123], record[124]) = (10, 25, 12, 0);
    let mut out = EXPORT_MAGIC.to_vec();
    out.extend_from_slice(&record);
    out.extend_from_slice(&[0x0E, 0x0E]);
    out.extend_from_slice(&replay);
    out
}

/// A game's key in an upload: room, table 0 and generation.
pub struct Game<'a> {
    pub room_id: &'a str,
    pub generation: u64,
    pub played_at: u64,
    pub fighters: [u8; 2],
}

impl<'a> Game<'a> {
    pub fn new(room_id: &'a str, generation: u64, played_at: u64) -> Self {
        Self {
            room_id,
            generation,
            played_at,
            fighters: FIGHTERS,
        }
    }

    pub fn body(&self, side: u8, name: &str, country: Option<&str>) -> Json_ {
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

pub async fn call(
    bridge: &Bridge,
    player: &Player,
    method: Method,
    path: &str,
    body: Option<Json_>,
) -> (StatusCode, Json_) {
    send(bridge, method, player.token(), "198.51.100.7", path, body).await
}

pub async fn upload(bridge: &Bridge, player: &Player, body: Json_) -> (StatusCode, Json_) {
    call(bridge, player, Method::POST, REPLAYS, Some(body)).await
}

/// Uploads and expects the bridge to take it.
pub async fn uploaded(bridge: &Bridge, player: &Player, body: Json_) -> Json_ {
    let (status, answer) = upload(bridge, player, body).await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    answer
}

pub async fn page(bridge: &Bridge, player: &Player, query: &str) -> Json_ {
    let path = format!("{REPLAYS}{query}");
    let (status, page) = call(bridge, player, Method::GET, &path, None).await;
    assert_eq!(status, StatusCode::OK, "{page}");
    page
}

/// The page the website reads, with no credential.
pub async fn public(bridge: &Bridge, query: &str) -> Json_ {
    let request = bridge.client.get(bridge.url(&format!("{PUBLIC}{query}")));
    let (status, page) = common::read(request.send().await.unwrap()).await;
    assert_eq!(status, StatusCode::OK, "{page}");
    page
}

pub fn listed(page: &Json_) -> Vec<String> {
    page["replays"]
        .as_array()
        .unwrap()
        .iter()
        .map(|entry| entry["id"].as_str().unwrap().to_owned())
        .collect()
}

pub async fn sharing(bridge: &Bridge, player: &Player, on: bool) -> (StatusCode, Json_) {
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
pub fn report_games(fake: &Supervisor, room_id: &str, games: &[(u64, &Player, &Player)]) {
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
pub async fn played(
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
pub fn private_room() -> String {
    "5".repeat(32)
}

pub fn now(bridge: &Bridge) -> u64 {
    bridge.state().now()
}

/// Gives Discord account `account` a settled rating in the middle of `band`.
pub async fn rate(bridge: &Bridge, account: &str, band: u8) {
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

/// Moves account `account`, which `rate` gave a rating, to the middle of `band`.
pub async fn rerate(bridge: &Bridge, account: &str, band: u8) {
    let account = account.to_owned();
    let rating = 1050.0 + 100.0 * f64::from(band);
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "UPDATE skill_ratings SET rating = ?2, band = ?3 WHERE discord_user_id = ?1",
                rusqlite::params![account, rating, band],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

/// Takes `player` out of ranking, so they show no class.
pub async fn leave_ranking(bridge: &Bridge, player: &Player) {
    let (status, out) = call(
        bridge,
        player,
        Method::PUT,
        "/v1/ranking/me",
        Some(json!({ "ranked": false })),
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{out}");
}

pub async fn count(bridge: &Bridge, table: &'static str) -> i64 {
    bridge
        .state()
        .db
        .read(move |tx| {
            Ok(tx.query_row(&format!("SELECT COUNT(*) FROM {table}"), [], |r| r.get(0))?)
        })
        .await
        .unwrap()
}

/// Where the bridge keeps the replay files.
pub fn bridge_folder(bridge: &Bridge) -> std::path::PathBuf {
    bridge
        .state()
        .config
        .database
        .parent()
        .unwrap()
        .join("replays")
}

/// The features the bridge offers.
pub async fn features(bridge: &Bridge) -> Vec<Json_> {
    let request = bridge.client.get(bridge.url("/v1/capabilities"));
    let (status, capabilities) = common::read(request.send().await.unwrap()).await;
    assert_eq!(status, StatusCode::OK, "{capabilities}");
    capabilities["features"].as_array().unwrap().clone()
}
