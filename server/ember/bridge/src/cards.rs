//! Card tokens (docs/design/FIGHTER_LEVELS.md, "The card token in the room
//! ticket"): an opaque, sealed name for a player's Discord account that a
//! public room shows its members, so any of them can ask the bridge for that
//! player's card later (`routes::levels`).
//!
//! A token is `c1.` and the unpadded base64url of the bridge's seal
//! (`Keys::seal`, a fresh nonce each time) over 17 bytes: the purpose byte,
//! the Discord user ID and the issue time, both little-endian u64. Only the
//! bridge reads one, and two tokens of the same account cannot be matched by
//! anyone else.
use ember_protocol::{
    encoding::{b64u, decode_b64u},
    levels::{CARD_PREFIX, CARD_VALID_SECS},
};

use crate::secrets::Keys;

/// Marks the sealed bytes as a card, so nothing else sealed under the same
/// key (room invitations, organizer grants) opens as one.
const PURPOSE: u8 = 0x01;
const PLAIN_LEN: usize = 17;
/// The nonce, the plain bytes and the tag.
const SEALED_LEN: usize = 24 + PLAIN_LEN + 16;

/// A token for Discord account `account`, issued at `now`. None for an
/// account that is not a Discord user ID as Discord writes one.
pub fn issue(keys: &Keys, account: &str, now: u64) -> Option<String> {
    let id = account
        .parse::<u64>()
        .ok()
        .filter(|id| id.to_string() == account)?;
    let mut plain = [0; PLAIN_LEN];
    plain[0] = PURPOSE;
    plain[1..9].copy_from_slice(&id.to_le_bytes());
    plain[9..].copy_from_slice(&now.to_le_bytes());
    Some(format!("{CARD_PREFIX}{}", b64u(&keys.seal(&plain))))
}

/// The Discord account `token` names, if it opens under `keys` as a card
/// and was issued at most `CARD_VALID_SECS` before `now`.
pub fn open(keys: &Keys, token: &str, now: u64) -> Option<String> {
    let sealed: [u8; SEALED_LEN] = decode_b64u(token.strip_prefix(CARD_PREFIX)?, "card").ok()?;
    let plain: [u8; PLAIN_LEN] = keys.open(&sealed)?.as_slice().try_into().ok()?;
    let [purpose, rest @ ..] = plain;
    let (id, issued_at) = rest.split_at(8);
    let id = u64::from_le_bytes(id.try_into().ok()?);
    let issued_at = u64::from_le_bytes(issued_at.try_into().ok()?);
    (purpose == PURPOSE && now.saturating_sub(issued_at) <= CARD_VALID_SECS).then(|| id.to_string())
}

#[cfg(test)]
mod tests {
    use ember_protocol::levels::is_card_token;

    use super::*;

    const NOW: u64 = 1_731_000_000;
    const ACCOUNT: &str = "123456789012345678";

    #[test]
    fn a_token_names_its_account_and_says_nothing_else() {
        let keys = Keys::generate();
        let token = issue(&keys, ACCOUNT, NOW).unwrap();
        assert!(is_card_token(&token), "{token}");
        assert_eq!(token.len(), CARD_PREFIX.len() + 76);
        assert_eq!(open(&keys, &token, NOW).as_deref(), Some(ACCOUNT));
        // Two joins, two tokens nobody else can match.
        let again = issue(&keys, ACCOUNT, NOW).unwrap();
        assert_ne!(token, again);
        assert_eq!(open(&keys, &again, NOW).as_deref(), Some(ACCOUNT));
        // The largest Discord ID round-trips.
        let largest = u64::MAX.to_string();
        let token = issue(&keys, &largest, NOW).unwrap();
        assert_eq!(open(&keys, &token, NOW), Some(largest));
    }

    #[test]
    fn a_token_opens_for_seven_days() {
        let keys = Keys::generate();
        let token = issue(&keys, ACCOUNT, NOW).unwrap();
        assert!(open(&keys, &token, NOW + CARD_VALID_SECS).is_some());
        assert!(open(&keys, &token, NOW + CARD_VALID_SECS + 1).is_none());
        // A clock that stepped back does not refuse it.
        assert!(open(&keys, &token, NOW - 60).is_some());
    }

    #[test]
    fn only_a_card_sealed_by_this_bridge_opens() {
        let keys = Keys::generate();
        let token = issue(&keys, ACCOUNT, NOW).unwrap();
        assert!(open(&Keys::generate(), &token, NOW).is_none());
        // Another purpose under the same key.
        let mut plain = [0; PLAIN_LEN];
        plain[0] = 0x02;
        plain[1..9].copy_from_slice(&123_u64.to_le_bytes());
        plain[9..].copy_from_slice(&NOW.to_le_bytes());
        let other = format!("{CARD_PREFIX}{}", b64u(&keys.seal(&plain)));
        assert!(is_card_token(&other));
        assert!(open(&keys, &other, NOW).is_none());
        // Sealed bytes of another length, such as a room invitation.
        let invitation = format!("{CARD_PREFIX}{}", b64u(&keys.seal(b"sf4e3:invitation")));
        assert!(open(&keys, &invitation, NOW).is_none());
        // A changed character, a missing prefix, nothing at all.
        let mut changed = token.clone().into_bytes();
        let last = changed.len() - 1;
        changed[last] = if changed[last] == b'A' { b'B' } else { b'A' };
        assert!(open(&keys, &String::from_utf8(changed).unwrap(), NOW).is_none());
        assert!(open(&keys, &token[CARD_PREFIX.len()..], NOW).is_none());
        assert!(open(&keys, CARD_PREFIX, NOW).is_none());
    }

    #[test]
    fn only_a_discord_user_id_gets_a_token() {
        let keys = Keys::generate();
        for account in ["", "abc", "-1", "+1", "01", "18446744073709551616"] {
            assert!(issue(&keys, account, NOW).is_none(), "{account:?}");
        }
        assert!(issue(&keys, "0", NOW).is_some());
    }
}
