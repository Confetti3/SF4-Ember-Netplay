//! A tournament set's replays (docs/design/TOURNAMENT_DESK.md, 4b) and the
//! public replay archive's games (docs/design/REPLAY_ARCHIVE.md): the
//! `.emberreplay` a player's game archived, as it is sent to the bridge,
//! and the set's pack the bridge serves.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

/// An `.emberreplay`: `EMBRPLY1`, the game's 125-byte slot record, the
/// slot's two bytes, then the replay itself, at most 51200 bytes
/// (`src/common/ReplaySlots.hxx`).
pub const MAGIC: &[u8; 8] = b"EMBRPLY1";
pub const HEAD: usize = 8 + RECORD + 2;
pub const MAX_REPLAY: usize = HEAD + LARGEST_BODY;
/// A pack: `EMBRPAK1`, the header's length (u32, little-endian), the header
/// as JSON, then each game's `.emberreplay` in the header's order.
pub const PACK_MAGIC: &[u8; 8] = b"EMBRPAK1";

const RECORD: usize = 125;
/// The game preallocates this much for a replay, so none is longer.
const LARGEST_BODY: usize = 51200;
/// The replay's own header: `#BRP`, the time it was played (a FILETIME at
/// 0x10) and each player's fighter (a dword at 0x20, the second player's
/// 0x150 after the first's).
const BODY_MAGIC: &[u8; 4] = b"#BRP";
const BODY_HEADER: usize = 0x174;
const BODY_FIGHTER: usize = 0x20;
const BODY_PLAYER: usize = 0x150;
const BODY_PLAYED: usize = 0x10;
/// In the record: the used flag, the replay's CRC, size and save time, and
/// the menu's two 34-byte players (each fighter at +16) after a header of 8
/// bytes at +47.
const USED: usize = 4;
const CRC: usize = 5;
const SIZE: usize = 9;
const SAVED: usize = 13;
const MENU_PLAYERS: usize = 47 + 8;
const MENU_PLAYER: usize = 34;
const MENU_FIGHTER: usize = 16;
/// Native fighter IDs are below this.
const FIGHTERS: u32 = 64;
/// The FILETIME of 1970-01-01, and its ticks per second.
const FILETIME_1970: u64 = 116_444_736_000_000_000;
const FILETIME_SECOND: u64 = 10_000_000;

fn u32_at(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())
}

/// CRC-32 (IEEE), as the game's replay records hold it.
pub fn crc32(bytes: &[u8]) -> u32 {
    !bytes.iter().fold(!0u32, |crc, &byte| {
        (0..8).fold(crc ^ u32::from(byte), |crc, _| {
            (crc >> 1) ^ (0xEDB8_8320 & (crc & 1).wrapping_neg())
        })
    })
}

/// A whole `.emberreplay`, checked as the game checks one before it imports
/// it (`WholeArchived` and `Describes` in `src/common/ReplaySlots.hxx`):
/// its record is used and names the size and CRC of the replay it wraps,
/// which has a whole header naming the record's two fighters.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Replay {
    /// The CRC-32 of the replay, as the record and the file's name have it.
    pub crc: u32,
    /// When the game saved it (unix seconds), from its record.
    pub saved_at: u32,
    /// The two fighters (native IDs), P1 first.
    pub fighters: [u8; 2],
    /// The SHA-256 of the replay without the record and slot bytes around
    /// it. Those hold the saving PC's slot number, so both players' copies
    /// of one game have the same digest, and two games whose CRCs collide
    /// do not.
    pub digest: [u8; 32],
}

impl Replay {
    pub fn parse(file: &[u8]) -> Option<Self> {
        if file.len() < HEAD || file.len() > MAX_REPLAY || &file[..8] != MAGIC {
            return None;
        }
        let record = &file[8..8 + RECORD];
        let body = &file[HEAD..];
        if record[USED] == 0
            || u32_at(record, SIZE) as usize != body.len()
            || body.len() < BODY_HEADER
            || &body[..4] != BODY_MAGIC
        {
            return None;
        }
        let fighters = [0, 1].map(|side| {
            let fighter = u32_at(body, BODY_FIGHTER + side * BODY_PLAYER);
            let named = record[MENU_PLAYERS + side * MENU_PLAYER + MENU_FIGHTER];
            (fighter < FIGHTERS && fighter == u32::from(named)).then_some(named)
        });
        let [Some(p1), Some(p2)] = fighters else {
            return None;
        };
        let crc = crc32(body);
        (u32_at(record, CRC) == crc).then(|| Self {
            crc,
            saved_at: u32_at(record, SAVED),
            fighters: [p1, p2],
            digest: Sha256::digest(body).into(),
        })
    }

    /// The CRC as the file's name and the bridge's path write it.
    pub fn crc_name(&self) -> String {
        format!("{:08x}", self.crc)
    }

    pub fn digest_hex(&self) -> String {
        self.digest
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect()
    }
}

/// The CRC an `.emberreplay`'s record names, as its file name and the
/// bridge's path write it; none for a file too short to hold a replay. It
/// reads the name only; `saved_at` checks the file.
pub fn crc_name(replay: &[u8]) -> Option<String> {
    (replay.len() > HEAD).then(|| format!("{:08x}", u32_at(replay, 8 + CRC)))
}

/// The save time (unix seconds) of a whole `.emberreplay` (`Replay::parse`)
/// whose CRC `crc` names, or none.
pub fn saved_at(crc: &str, replay: &[u8]) -> Option<u32> {
    Replay::parse(replay)
        .filter(|parsed| parsed.crc_name() == crc)
        .map(|parsed| parsed.saved_at)
}

/// The two fighters (native IDs) of a whole `.emberreplay`, or none.
pub fn fighters(replay: &[u8]) -> [Option<u8>; 2] {
    Replay::parse(replay).map_or([None; 2], |parsed| parsed.fighters.map(Some))
}

/// What the sending game says of a game for the replay archive
/// (docs/design/REPLAY_ARCHIVE.md): the two sides' names as it named them,
/// and its own side and country. The bridge checks each.
#[derive(Serialize, Deserialize, Default, Clone, Debug)]
pub struct Sides {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub a: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub b: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub side: Option<u8>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
}

impl Sides {
    /// The query pairs of the fields it has.
    pub fn pairs(&self) -> Vec<(&'static str, String)> {
        [
            ("a", self.a.clone()),
            ("b", self.b.clone()),
            ("side", self.side.map(|side| side.to_string())),
            ("country", self.country.clone()),
        ]
        .into_iter()
        .filter_map(|(key, value)| Some((key, value?)))
        .collect()
    }
}

/// A made-up `.emberreplay` for tests, laid out as the game exports a
/// Versus battle's slot (`ExportFromReplayAlone`): slot 300's record, the
/// slot bytes `0E 0E`, and a replay whose header names `fighters` and was
/// played at `saved_at`, followed by `payload`.
pub fn made_up(payload: &[u8], saved_at: u32, fighters: [u8; 2]) -> Vec<u8> {
    let mut body = vec![0; BODY_HEADER];
    body[..4].copy_from_slice(BODY_MAGIC);
    let played = FILETIME_1970 + u64::from(saved_at) * FILETIME_SECOND;
    body[BODY_PLAYED..BODY_PLAYED + 8].copy_from_slice(&played.to_le_bytes());
    for (side, fighter) in fighters.into_iter().enumerate() {
        let at = BODY_FIGHTER + side * BODY_PLAYER;
        body[at..at + 4].copy_from_slice(&u32::from(fighter).to_le_bytes());
    }
    body.extend_from_slice(payload);
    let mut record = [0u8; RECORD];
    record[..4].copy_from_slice(&300u32.to_le_bytes());
    record[USED] = 1;
    record[CRC..CRC + 4].copy_from_slice(&crc32(&body).to_le_bytes());
    record[SIZE..SIZE + 4].copy_from_slice(&(body.len() as u32).to_le_bytes());
    record[SAVED..SAVED + 4].copy_from_slice(&saved_at.to_le_bytes());
    // The menu's date and time: a Versus battle (7), then the year, month
    // and day, and the hour and minute at the record's end.
    let date = crate::event::rfc3339(u64::from(saved_at));
    let part = |range: std::ops::Range<usize>| date[range].parse::<u16>().unwrap();
    record[47..51].copy_from_slice(&[0x02, 0x0E, 0x00, 0x07]);
    record[51..53].copy_from_slice(&part(0..4).to_le_bytes());
    record[53] = part(5..7) as u8;
    record[54] = part(8..10) as u8;
    for (side, fighter) in fighters.into_iter().enumerate() {
        record[MENU_PLAYERS + side * MENU_PLAYER + MENU_FIGHTER] = fighter;
        record[MENU_PLAYERS + side * MENU_PLAYER + 22] = 8;
    }
    record[123] = part(11..13) as u8;
    record[124] = part(14..16) as u8;
    let mut out = MAGIC.to_vec();
    out.extend_from_slice(&record);
    out.extend_from_slice(&[0x0E, 0x0E]);
    out.extend_from_slice(&body);
    out
}

/// `made_up` with fighters 1 and 2.
pub fn wrap(payload: &[u8], saved_at: u32) -> Vec<u8> {
    made_up(payload, saved_at, [1, 2])
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAVED_AT: u32 = 1_800_000_000;

    fn game() -> Vec<u8> {
        made_up(b"a game", SAVED_AT, [20, 34])
    }

    /// `file` with the four bytes at `at` set to `value`.
    fn with_u32(mut file: Vec<u8>, at: usize, value: u32) -> Vec<u8> {
        file[at..at + 4].copy_from_slice(&value.to_le_bytes());
        file
    }

    /// `file` with its record's CRC made right for its body again.
    fn recrc(mut file: Vec<u8>) -> Vec<u8> {
        let crc = crc32(&file[HEAD..]);
        file[8 + CRC..8 + CRC + 4].copy_from_slice(&crc.to_le_bytes());
        file
    }

    #[test]
    fn crc32_is_the_ieee_one() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(crc32(b""), 0);
    }

    #[test]
    fn a_whole_replay_reads_with_its_record_and_digest() {
        let file = game();
        let replay = Replay::parse(&file).unwrap();
        assert_eq!(replay.saved_at, SAVED_AT);
        assert_eq!(replay.fighters, [20, 34]);
        assert_eq!(replay.crc, crc32(&file[HEAD..]));
        assert_eq!(replay.digest_hex().len(), 64);
        assert_eq!(crc_name(&file), Some(replay.crc_name()));
        assert_eq!(saved_at(&replay.crc_name(), &file), Some(SAVED_AT));
        assert_eq!(fighters(&file), [Some(20), Some(34)]);
        // The saving PC's slot number is no part of the game.
        let elsewhere = with_u32(file.clone(), 8, 284);
        assert_eq!(Replay::parse(&elsewhere), Some(replay));
    }

    #[test]
    fn only_a_whole_replay_with_its_crc_reads() {
        let file = game();
        let crc = crc_name(&file).unwrap();
        assert_eq!(saved_at(&crc.to_uppercase(), &file), None);
        assert_eq!(saved_at("00000000", &file), None);
        let mut changed = file.clone();
        *changed.last_mut().unwrap() ^= 1;
        assert_eq!(saved_at(&crc, &changed), None);
        assert_eq!(saved_at(&crc, &file[..HEAD]), None);
        assert_eq!(crc_name(&file[..HEAD]), None);
        assert_eq!(fighters(&file[..HEAD]), [None, None]);
    }

    /// Each check the game makes before it imports an export, broken once,
    /// with the record's CRC made right again wherever the body changed.
    #[test]
    fn a_replay_its_record_does_not_describe_is_refused() {
        let file = game();
        let body_len = (file.len() - HEAD) as u32;
        let mut unused = file.clone();
        unused[8 + USED] = 0;
        let mut torn = file.clone();
        torn.pop();
        let mut not_brp = file.clone();
        not_brp[HEAD] = b'$';
        let mut other_fighter = file.clone();
        other_fighter[8 + MENU_PLAYERS + MENU_PLAYER + MENU_FIGHTER] = 35;
        let mut wrapper = file.clone();
        wrapper[..8].copy_from_slice(b"EMBRPLY2");
        let short_header = {
            let mut file = made_up(b"", SAVED_AT, [20, 34]);
            file.pop();
            with_u32(recrc(file), 8 + SIZE, (BODY_HEADER - 1) as u32)
        };
        let refused = [
            ("unused record", unused),
            (
                "size one short",
                with_u32(file.clone(), 8 + SIZE, body_len - 1),
            ),
            (
                "size one long",
                with_u32(file.clone(), 8 + SIZE, body_len + 1),
            ),
            ("torn body", recrc(torn)),
            ("not a #BRP replay", recrc(not_brp)),
            ("header too short", short_header),
            ("record names another fighter", other_fighter),
            (
                "fighter out of the roster",
                recrc(with_u32(file.clone(), HEAD + BODY_FIGHTER, 64)),
            ),
            (
                "record names another crc",
                with_u32(file.clone(), 8 + CRC, 0),
            ),
            ("other wrapper", wrapper),
            (
                "too long",
                made_up(&vec![0; LARGEST_BODY], SAVED_AT, [20, 34]),
            ),
        ];
        for (what, file) in refused {
            assert_eq!(Replay::parse(&file), None, "{what}");
        }
        // The header alone reads, and so does the largest replay the game saves.
        assert!(Replay::parse(&made_up(b"", SAVED_AT, [0, 63])).is_some());
        let largest = made_up(&vec![0; LARGEST_BODY - BODY_HEADER], SAVED_AT, [1, 2]);
        assert_eq!(largest.len(), MAX_REPLAY);
        assert!(Replay::parse(&largest).is_some());
    }

    #[test]
    fn a_made_up_record_has_the_games_date() {
        // 2027-01-15 08:00 UTC.
        let file = made_up(b"", SAVED_AT, [1, 2]);
        let record = &file[8..8 + RECORD];
        assert_eq!(u16::from_le_bytes([record[51], record[52]]), 2027);
        assert_eq!(
            (record[53], record[54], record[123], record[124]),
            (1, 15, 8, 0)
        );
        let played = u64::from_le_bytes(
            file[HEAD + BODY_PLAYED..HEAD + BODY_PLAYED + 8]
                .try_into()
                .unwrap(),
        );
        assert_eq!(
            (played - FILETIME_1970) / FILETIME_SECOND,
            u64::from(SAVED_AT)
        );
    }
}
