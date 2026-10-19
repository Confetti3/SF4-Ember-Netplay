//! A tournament set's replays (docs/design/TOURNAMENT_DESK.md, 4b): the
//! `.emberreplay` a player's game archived, as it is sent to the bridge,
//! and the set's pack the bridge serves.

/// An `.emberreplay`: `EMBRPLY1`, the game's 125-byte record (the replay's
/// CRC at +5, its save time at +13), two bytes, then the replay itself, at
/// most 51200 bytes (`src/common/ReplaySlots.hxx`).
pub const MAGIC: &[u8; 8] = b"EMBRPLY1";
pub const HEAD: usize = 8 + 125 + 2;
pub const MAX_REPLAY: usize = HEAD + 51200;
/// A pack: `EMBRPAK1`, the header's length (u32, little-endian), the header
/// as JSON, then each game's `.emberreplay` in the header's order.
pub const PACK_MAGIC: &[u8; 8] = b"EMBRPAK1";

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

/// The CRC an `.emberreplay`'s record names, as its file name and the
/// bridge's path write it; none for a file too short to hold a replay.
pub fn crc_name(replay: &[u8]) -> Option<String> {
    (replay.len() > HEAD).then(|| format!("{:08x}", u32_at(replay, 8 + 5)))
}

/// The save time (unix seconds) of a whole `.emberreplay` whose record and
/// body both have the CRC `crc` names, or none.
pub fn saved_at(crc: &str, replay: &[u8]) -> Option<u32> {
    let named = (crc.len() == 8 && crc.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')))
        .then(|| u32::from_str_radix(crc, 16).ok())
        .flatten()?;
    (replay.len() > HEAD
        && replay.len() <= MAX_REPLAY
        && &replay[..8] == MAGIC
        && u32_at(replay, 8 + 5) == named
        && crc32(&replay[HEAD..]) == named)
        .then(|| u32_at(replay, 8 + 13))
}

/// A made-up `.emberreplay` around `body`, saved at `saved_at`, for tests.
pub fn wrap(body: &[u8], saved_at: u32) -> Vec<u8> {
    let mut out = MAGIC.to_vec();
    out.resize(HEAD, 0);
    out[8 + 5..8 + 9].copy_from_slice(&crc32(body).to_le_bytes());
    out[8 + 13..8 + 17].copy_from_slice(&saved_at.to_le_bytes());
    out.extend_from_slice(body);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32_is_the_ieee_one() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(crc32(b""), 0);
    }

    #[test]
    fn only_a_whole_replay_with_its_crc_reads() {
        let replay = wrap(b"a game", 1_800_000_000);
        let crc = crc_name(&replay).unwrap();
        assert_eq!(saved_at(&crc, &replay), Some(1_800_000_000));
        assert_eq!(saved_at(&crc.to_uppercase(), &replay), None);
        assert_eq!(saved_at("00000000", &replay), None);
        let mut changed = replay.clone();
        *changed.last_mut().unwrap() ^= 1;
        assert_eq!(saved_at(&crc, &changed), None);
        assert_eq!(saved_at(&crc, &replay[..HEAD]), None);
        assert_eq!(crc_name(&replay[..HEAD]), None);
    }
}
