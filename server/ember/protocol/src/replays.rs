//! The replay channel (`docs/design/REPLAY_CHANNEL.md`): players' room games
//! shared with everyone, listed with both players' names, fighters and
//! classes, and each player's choice to share them.
use serde::{Deserialize, Serialize};

use crate::{
    Error, Result,
    encoding::{b64, decode_b64},
    rooms::{is_false, is_player_name},
    skill::SkillBadge,
};

/// The capability a bridge with the replay channel offers.
pub const REPLAY_CHANNEL_FEATURE: &str = "replay_channel";
/// `POST` a `ReplayUpload`, answered with an `UploadAnswer`; `GET` a
/// `ReplayPage` for the caller (query in `ListQuery`).
pub const REPLAYS_PATH: &str = "/v1/replays";
/// `GET` the caller's `ReplaySharing`; `PUT` a `SharingChoice`, answered with it.
pub const SHARING_PATH: &str = "/v1/replays/me";
/// `GET` the `ReplayPage` anyone may read (no credential).
pub const PUBLIC_PATH: &str = "/v1/replays/public";

/// `GET` one replay's `.emberreplay` bytes.
pub fn file_path(id: &str) -> String {
    format!("/v1/replays/{id}/file")
}

/// The game's largest replay file and the export header before it: an
/// `.emberreplay` is the magic, the slot record and the slot's two bytes,
/// then the replay (`src/common/ReplaySlots.hxx`).
pub const MAX_REPLAY: usize = 51_200;
pub const EXPORT_MAGIC: &[u8; 8] = b"EMBRPLY1";
pub const RECORD_BYTES: usize = 125;
pub const EXPORT_HEADER: usize = 8 + RECORD_BYTES + 2;
pub const MAX_EXPORT: usize = EXPORT_HEADER + MAX_REPLAY;
/// What the replay header must hold for its two fighters to be read.
const REPLAY_HEADER: usize = 0x174;
/// The most entries one page lists.
pub const PAGE_SIZE: usize = 50;
/// The longest name filter.
pub const MAX_NAME_FILTER: usize = 32;

/// Refusal reasons of the replay routes. The ranking routes' `no_discord_account`
/// (`skill::NO_ACCOUNT`) also applies: a choice to share needs an account.
pub const REPLAY_INVALID: &str = "replay_invalid";
pub const NOT_YOUR_GAME: &str = "replay_not_your_game";
pub const UPLOAD_LIMIT: &str = "replay_upload_limit";
pub const REPLAY_NOT_FOUND: &str = "replay_not_found";

/// An entry's id: 16 random bytes in lower-case hex.
pub fn is_replay_id(id: &str) -> bool {
    id.len() == 32
        && id
            .bytes()
            .all(|byte| matches!(byte, b'0'..=b'9' | b'a'..=b'f'))
}

/// A room's id as the bridge and room hosts write it: 16 bytes in lower-case hex.
pub fn is_room_id(id: &str) -> bool {
    is_replay_id(id) && id.bytes().any(|byte| byte != b'0')
}

fn read_u32(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

/// CRC-32 (IEEE), as the game's records name a replay.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &byte in data {
        crc ^= u32::from(byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320 & 0u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}

/// What the bridge reads from an `.emberreplay` itself.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ExportInfo {
    /// Each side's native fighter id, from the replay header.
    pub fighters: [u8; 2],
    /// When the game was played, seconds since 1970 (UTC), from the header.
    pub played_at: u64,
}

/// Reads an `.emberreplay` the way the game's import checks one: the magic, a
/// used record naming the replay's size and CRC with the header's fighters,
/// a title of at most 21 printable characters or zero bytes (the game's own
/// records leave it blank), and a whole replay header.
pub fn read_export(export: &[u8]) -> Result<ExportInfo> {
    let invalid = Err(Error::InvalidField("replay"));
    if export.len() < EXPORT_HEADER + REPLAY_HEADER
        || export.len() > MAX_EXPORT
        || &export[..8] != EXPORT_MAGIC
    {
        return invalid;
    }
    let record = &export[8..8 + RECORD_BYTES];
    let replay = &export[EXPORT_HEADER..];
    if &replay[..4] != b"#BRP" {
        return invalid;
    }
    let fighter = |side: usize| read_u32(replay, 0x20 + side * 0x150);
    let fighters = [fighter(0), fighter(1)];
    if fighters.iter().any(|&fighter| fighter >= 64) {
        return invalid;
    }
    let title = read_u32(record, 22) as usize;
    let title_valid = title <= 21
        && record[26..26 + title]
            .iter()
            .all(|&byte| byte == 0 || (0x20..0x7F).contains(&byte));
    let described = record[4] != 0
        && read_u32(record, 5) == crc32(replay)
        && read_u32(record, 9) as usize == replay.len()
        // The record keeps each side's fighter at +71 and +105 (ReadRecordInfo).
        && u32::from(record[71]) == fighters[0]
        && u32::from(record[105]) == fighters[1];
    if !title_valid || !described {
        return invalid;
    }
    let filetime = u64::from(read_u32(replay, 0x10)) | (u64::from(read_u32(replay, 0x14)) << 32);
    let played_at = filetime.saturating_sub(116_444_736_000_000_000) / 10_000_000;
    Ok(ExportInfo {
        fighters: [fighters[0] as u8, fighters[1] as u8],
        played_at,
    })
}

/// `POST /v1/replays`: one game the caller fought in an Ember room, keyed as
/// the room host reports games (room, table, generation). `side` is the
/// caller's player slot (0 or 1); `name` and `country` are how the caller's
/// own game shows them, never the opponent.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ReplayUpload {
    pub room_id: String,
    pub table: u8,
    pub generation: u64,
    pub side: u8,
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    /// The `.emberreplay`, base64.
    pub replay: String,
}

/// The JSON body a whole upload may take: the export in base64 and the rest.
pub const MAX_UPLOAD_BODY: usize = 96 * 1024;

pub fn is_country(country: &str) -> bool {
    country.len() == 2 && country.bytes().all(|byte| byte.is_ascii_uppercase())
}

impl ReplayUpload {
    pub fn new(
        room_id: String,
        table: u8,
        generation: u64,
        side: u8,
        name: String,
        country: Option<String>,
        export: &[u8],
    ) -> Self {
        Self {
            room_id,
            table,
            generation,
            side,
            name,
            country,
            replay: b64(export),
        }
    }

    /// Checks every field and answers the export's bytes and what they hold.
    pub fn check(&self) -> Result<(Vec<u8>, ExportInfo)> {
        if !is_room_id(&self.room_id) {
            return Err(Error::InvalidField("room_id"));
        }
        if self.side > 1 {
            return Err(Error::InvalidField("side"));
        }
        if !is_player_name(&self.name) {
            return Err(Error::InvalidField("name"));
        }
        if !self.country.as_deref().is_none_or(is_country) {
            return Err(Error::InvalidField("country"));
        }
        if self.replay.len() > MAX_EXPORT.div_ceil(3) * 4 {
            return Err(Error::InvalidField("replay"));
        }
        let export = decode_b64(&self.replay, "replay")?;
        let info = read_export(&export)?;
        Ok((export, info))
    }
}

/// What became of an upload.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum UploadState {
    /// Listed on the channel.
    Published,
    /// Kept until the bridge knows the other side (a private room's other
    /// upload), then listed or dropped.
    Held,
    /// Kept out of the channel: a player does not share, or has no account.
    NotShared,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct UploadAnswer {
    pub state: UploadState,
    /// The entry, once there is one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub id: Option<String>,
}

impl UploadAnswer {
    pub fn check(&self) -> Result<()> {
        if self.id.as_deref().is_none_or(is_replay_id) {
            Ok(())
        } else {
            Err(Error::InvalidField("id"))
        }
    }
}

/// `PUT /v1/replays/me`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SharingChoice {
    pub sharing: bool,
}

/// The caller's choice (`GET /v1/replays/me`). Sharing is on until a player
/// turns it off.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "sharing", rename_all = "snake_case", deny_unknown_fields)]
pub enum ReplaySharing {
    /// No Discord account is connected to the caller's Ember ID, so their
    /// replays are never listed.
    NoAccount,
    On,
    Off,
}

/// Filters of a page (`GET /v1/replays` and `/v1/replays/public` query).
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ListQuery {
    /// A native fighter id either side played.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fighter: Option<u8>,
    /// A class (1 to 10) either side held.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub band: Option<u8>,
    /// Part of either side's name, compared without case.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// Entries listed after this one (the last of the previous page).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub before: Option<String>,
}

impl ListQuery {
    pub fn check(&self) -> Result<()> {
        if self.fighter.is_some_and(|fighter| fighter >= 64) {
            return Err(Error::InvalidField("fighter"));
        }
        if self
            .band
            .is_some_and(|band| !(1..=crate::skill::SKILL_BANDS).contains(&band))
        {
            return Err(Error::InvalidField("band"));
        }
        let name_valid = self.name.as_deref().is_none_or(|name| {
            !name.is_empty()
                && name.chars().count() <= MAX_NAME_FILTER
                && !name.chars().any(char::is_control)
        });
        if !name_valid {
            return Err(Error::InvalidField("name"));
        }
        if !self.before.as_deref().is_none_or(is_replay_id) {
            return Err(Error::InvalidField("before"));
        }
        Ok(())
    }
}

/// One side of a listed game.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ChannelSide {
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    pub fighter: u8,
    /// The class the side held when the game was listed; absent for a player
    /// who does not take part in ranking or had none yet.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub badge: Option<SkillBadge>,
    /// The caller (in-game page only).
    #[serde(default, skip_serializing_if = "is_false")]
    pub own: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RoomKind {
    Public,
    Private,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ChannelReplay {
    pub id: String,
    /// Seconds since 1970 (UTC).
    pub played_at: u64,
    pub room: RoomKind,
    pub sides: [ChannelSide; 2],
    /// The winning side, when the room host reported it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub winner: Option<u8>,
    /// The file's size in bytes.
    pub size: u32,
}

/// A page of the channel, newest first.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ReplayPage {
    pub replays: Vec<ChannelReplay>,
    /// Whether older entries follow the last one.
    #[serde(default, skip_serializing_if = "is_false")]
    pub more: bool,
}

impl ReplayPage {
    pub fn check(&self) -> Result<()> {
        if self.replays.len() > PAGE_SIZE {
            return Err(Error::InvalidField("replays"));
        }
        let ordered = self
            .replays
            .windows(2)
            .all(|pair| pair[0].played_at >= pair[1].played_at);
        if !ordered {
            return Err(Error::InvalidField("replays"));
        }
        for replay in &self.replays {
            if !is_replay_id(&replay.id)
                || replay.winner.is_some_and(|winner| winner > 1)
                || replay.size as usize > MAX_EXPORT
            {
                return Err(Error::InvalidField("replays"));
            }
            for side in &replay.sides {
                let valid = is_player_name(&side.name)
                    && side.country.as_deref().is_none_or(is_country)
                    && side.fighter < 64;
                if !valid {
                    return Err(Error::InvalidField("sides"));
                }
                side.badge.as_ref().map_or(Ok(()), SkillBadge::check)?;
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A minimal export of a replay of fighters 39 and 23 played at `time`.
    pub fn export(time: u64) -> Vec<u8> {
        let mut replay = vec![0u8; 0x400];
        replay[..4].copy_from_slice(b"#BRP");
        let filetime = time * 10_000_000 + 116_444_736_000_000_000;
        replay[0x10..0x18].copy_from_slice(&filetime.to_le_bytes());
        replay[0x20..0x24].copy_from_slice(&39u32.to_le_bytes());
        replay[0x170..0x174].copy_from_slice(&23u32.to_le_bytes());
        let mut record = vec![0u8; RECORD_BYTES];
        record[4] = 1;
        record[5..9].copy_from_slice(&crc32(&replay).to_le_bytes());
        record[9..13].copy_from_slice(&(replay.len() as u32).to_le_bytes());
        record[22..26].copy_from_slice(&16u32.to_le_bytes());
        record[26..42].copy_from_slice(b"2026-10-25 12:00");
        record[71] = 39;
        record[105] = 23;
        let mut out = EXPORT_MAGIC.to_vec();
        out.extend_from_slice(&record);
        out.extend_from_slice(&[0x0E, 0x0E]);
        out.extend_from_slice(&replay);
        out
    }

    #[test]
    fn an_export_names_its_fighters_and_time() {
        let info = read_export(&export(1_761_393_600)).unwrap();
        assert_eq!(info.fighters, [39, 23]);
        assert_eq!(info.played_at, 1_761_393_600);
    }

    #[test]
    fn an_export_that_does_not_describe_its_replay_is_refused() {
        let good = export(1_761_393_600);
        let mut changed_body = good.clone();
        *changed_body.last_mut().unwrap() ^= 1;
        let mut other_fighter = good.clone();
        other_fighter[8 + 105] = 24;
        let mut long_title = good.clone();
        long_title[8 + 22] = 22;
        let mut magic = good.clone();
        magic[0] = b'X';
        let mut too_big = good.clone();
        too_big.resize(MAX_EXPORT + 1, 0);
        for bad in [changed_body, other_fighter, long_title, magic, too_big] {
            assert_eq!(read_export(&bad), Err(Error::InvalidField("replay")));
        }
    }

    #[test]
    fn an_upload_checks_every_field() {
        let upload = ReplayUpload::new(
            "0123456789abcdef0123456789abcdef".into(),
            0,
            3,
            1,
            "Player 1".into(),
            Some("GB".into()),
            &export(1_761_393_600),
        );
        assert!(upload.check().is_ok());
        let bad = [
            ReplayUpload {
                room_id: "0".repeat(32),
                ..upload.clone()
            },
            ReplayUpload {
                side: 2,
                ..upload.clone()
            },
            ReplayUpload {
                name: " Player 1".into(),
                ..upload.clone()
            },
            ReplayUpload {
                country: Some("gb".into()),
                ..upload.clone()
            },
            ReplayUpload {
                replay: "AAAA".into(),
                ..upload.clone()
            },
        ];
        for upload in bad {
            assert!(upload.check().is_err(), "{upload:?}");
        }
    }

    #[test]
    fn sharing_reads_as_one_of_three_shapes() {
        let read = |text: &str| serde_json::from_str::<ReplaySharing>(text).unwrap();
        assert_eq!(read(r#"{"sharing":"on"}"#), ReplaySharing::On);
        assert_eq!(read(r#"{"sharing":"off"}"#), ReplaySharing::Off);
        assert_eq!(
            read(r#"{"sharing":"no_account"}"#),
            ReplaySharing::NoAccount
        );
    }

    #[test]
    fn a_page_is_newest_first_and_bounded() {
        let side = |name: &str| ChannelSide {
            name: name.into(),
            country: None,
            fighter: 39,
            badge: Some(SkillBadge::class(4, true)),
            own: false,
        };
        let entry = |id: char, played_at| ChannelReplay {
            id: id.to_string().repeat(32),
            played_at,
            room: RoomKind::Public,
            sides: [side("Player 1"), side("Player 2")],
            winner: Some(0),
            size: 9000,
        };
        let page = ReplayPage {
            replays: vec![entry('b', 20), entry('a', 10)],
            more: true,
        };
        assert!(page.check().is_ok());
        let backwards = ReplayPage {
            replays: vec![entry('a', 10), entry('b', 20)],
            more: false,
        };
        assert!(backwards.check().is_err());
        assert!(
            ListQuery {
                band: Some(11),
                ..Default::default()
            }
            .check()
            .is_err()
        );
        assert!(
            ListQuery {
                name: Some("ry".into()),
                fighter: Some(1),
                ..Default::default()
            }
            .check()
            .is_ok()
        );
    }
}
