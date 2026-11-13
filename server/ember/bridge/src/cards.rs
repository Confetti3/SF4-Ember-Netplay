//! Card tokens (docs/design/FIGHTER_LEVELS.md, "The card token in the room
//! ticket"): an opaque, sealed name for a player's Discord account that a
//! public room shows its members, so any of them can ask the bridge for that
//! player's card later (`routes::levels`).
//!
//! The same module keeps what a card says beyond its points
//! (docs/design/PROFILE.md): the `player_cards` row of each Discord account,
//! which `PUT /v1/cards/me` and a friends sync write, which a card look-up
//! and a search read.
//!
//! A token is `c1.` and the unpadded base64url of the bridge's seal
//! (`Keys::seal`, a fresh nonce each time) over 17 bytes: the purpose byte,
//! the Discord user ID and the issue time, both little-endian u64. Only the
//! bridge reads one, and two tokens of the same account cannot be matched by
//! anyone else.
use ember_protocol::{
    encoding::{b64u, decode_b64u},
    levels::{CARD_PREFIX, CARD_VALID_SECS, CardSave, PlayerCard, Style, Worn},
    skill::{Listing, SkillBadge},
};
use rusqlite::{OptionalExtension, Transaction, params};

use crate::{error::Result, friends::Classes, secrets::Keys, skill};

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

/// What `player_cards` holds of an account.
pub struct Stored {
    pub name: String,
    pub country: Option<String>,
    pub main: Option<u8>,
    /// Unix seconds the account came to Ember; served as the month's first.
    pub since: u64,
    pub worn: Worn,
    pub pins: Vec<String>,
    pub style: Style,
}

/// `account`'s row, once it has one.
pub fn stored(tx: &Transaction<'_>, account: &str) -> Result<Option<Stored>> {
    Ok(tx
        .query_row(
            "SELECT name, country, main_fighter, since, worn_title, worn_icon, pins,
                    device, link, region, looking
             FROM player_cards WHERE discord_user_id = ?1",
            [account],
            |row| {
                let pins: String = row.get(6)?;
                Ok(Stored {
                    name: row.get(0)?,
                    country: row.get(1)?,
                    main: row.get(2)?,
                    since: row.get(3)?,
                    worn: Worn {
                        title: row.get(4)?,
                        icon: row.get(5)?,
                    },
                    pins: pins
                        .split(',')
                        .filter(|pin| !pin.is_empty())
                        .map(str::to_owned)
                        .collect(),
                    style: Style {
                        device: row.get(7)?,
                        link: row.get(8)?,
                        region: row.get(9)?,
                        looking: row.get(10)?,
                    },
                })
            },
        )
        .optional()?)
}

/// Replaces `account`'s card with what its player chose to show. The row is
/// made here the first time, from when the account connected; `since` is
/// never moved after.
pub fn save(tx: &Transaction<'_>, account: &str, card: &CardSave, now: u64) -> Result<()> {
    tx.execute(
        "INSERT INTO player_cards (discord_user_id, name, country, main_fighter, since,
             worn_title, worn_icon, pins, device, link, region, looking, updated_at)
         VALUES (?1, ?2, ?3, ?4,
             COALESCE((SELECT connected_at FROM discord_accounts WHERE user_id = ?1), ?5),
             ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?5)
         ON CONFLICT (discord_user_id) DO UPDATE SET name = ?2, country = ?3, main_fighter = ?4,
             worn_title = ?6, worn_icon = ?7, pins = ?8, device = ?9, link = ?10, region = ?11,
             looking = ?12, updated_at = ?5",
        params![
            account,
            card.listing.name,
            card.listing.country,
            card.main,
            now,
            card.worn.title,
            card.worn.icon,
            card.pins.join(","),
            card.style.device,
            card.style.link,
            card.style.region,
            card.style.looking,
        ],
    )?;
    Ok(())
}

/// Keeps the name, flag and main fighter a friends sync sent in `account`'s
/// card, making the row when there is none, and writing only what changed:
/// the game syncs every half minute. What `save` stored is left alone.
pub fn note_listing(
    tx: &Transaction<'_>,
    account: &str,
    listing: &Listing,
    main: Option<u8>,
    now: u64,
) -> Result<()> {
    tx.execute(
        "INSERT INTO player_cards (discord_user_id, name, country, main_fighter, since, updated_at)
         VALUES (?1, ?2, ?3, ?4,
             COALESCE((SELECT connected_at FROM discord_accounts WHERE user_id = ?1), ?5), ?5)
         ON CONFLICT (discord_user_id) DO UPDATE SET name = excluded.name,
             country = excluded.country, main_fighter = excluded.main_fighter,
             updated_at = excluded.updated_at
         WHERE name COLLATE BINARY IS NOT excluded.name COLLATE BINARY
            OR country IS NOT excluded.country OR main_fighter IS NOT excluded.main_fighter",
        params![account, listing.name, listing.country, main, now],
    )?;
    Ok(())
}

/// `card` with what `stored` says of its player: the name, flag and main, the
/// month they came, the families they wear and show, how they play.
pub fn describe(card: PlayerCard, stored: Stored) -> PlayerCard {
    PlayerCard {
        name: Some(stored.name),
        country: stored.country,
        main: stored.main,
        since: Some(month_start(stored.since)),
        worn: stored.worn,
        pins: stored.pins,
        style: stored.style,
        ..card
    }
}

/// The class `account` shows now, as a friends state shows it: none while
/// the bridge hides classes or when the player does not take part.
pub fn badge(
    tx: &Transaction<'_>,
    account: &str,
    classes: &Classes,
    now: u64,
) -> Result<Option<SkillBadge>> {
    if !classes.shown {
        return Ok(None);
    }
    skill::last_shown(tx, account, &classes.developers, now)
}

/// A player a search found, as stored.
pub struct Found {
    pub account: String,
    pub name: String,
    pub country: Option<String>,
    pub main: Option<u8>,
    pub since: u64,
}

fn found_of(row: &rusqlite::Row<'_>) -> rusqlite::Result<Found> {
    Ok(Found {
        account: row.get(0)?,
        name: row.get(1)?,
        country: row.get(2)?,
        main: row.get(3)?,
        since: row.get(4)?,
    })
}

/// Up to `scan` accounts with a Discord account connected whose name is
/// `name` in any case (ASCII; what SQLite's NOCASE folds), then those that
/// begin with it, each most recently updated first. A caller drops the ones
/// it may not list and keeps what it wants.
pub fn find_by_name(tx: &Transaction<'_>, name: &str, scan: usize) -> Result<Vec<Found>> {
    let pattern = format!(
        "{}%",
        name.replace('\\', "\\\\")
            .replace('%', "\\%")
            .replace('_', "\\_")
    );
    let found = tx
        .prepare(
            "SELECT discord_user_id, name, country, main_fighter, since FROM player_cards
             WHERE name LIKE ?1 ESCAPE '\\'
               AND EXISTS (SELECT 1 FROM discord_accounts d WHERE d.user_id = player_cards.discord_user_id)
             ORDER BY name = ?2 DESC, updated_at DESC, discord_user_id LIMIT ?3",
        )?
        .query_map(params![pattern, name, scan as i64], found_of)?
        .collect::<rusqlite::Result<_>>()?;
    Ok(found)
}

/// `account`'s row as a search hit, when it has one and a Discord account
/// is connected.
pub fn found(tx: &Transaction<'_>, account: &str) -> Result<Option<Found>> {
    Ok(tx
        .query_row(
            "SELECT discord_user_id, name, country, main_fighter, since FROM player_cards
             WHERE discord_user_id = ?1
               AND EXISTS (SELECT 1 FROM discord_accounts d WHERE d.user_id = ?1)",
            [account],
            found_of,
        )
        .optional()?)
}

/// The first second, in UTC, of the month `secs` falls in. A card says only
/// the month a player came, not the day.
pub fn month_start(secs: u64) -> u64 {
    // Howard Hinnant's civil-from-days, for the day of the month.
    let days = (secs / 86_400) as i64;
    let shifted = days + 719_468;
    let era = shifted.div_euclid(146_097);
    let day_of_era = shifted.rem_euclid(146_097);
    let year_of_era =
        (day_of_era - day_of_era / 1_460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_from_march = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_from_march + 2) / 5 + 1;
    ((days - (day - 1)) * 86_400) as u64
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
    fn a_month_starts_on_its_first_second_in_utc() {
        const DAY: u64 = 86_400;
        // 2025-06-01 00:00:00, its last second, and a day in its middle.
        let june = 20_240 * DAY;
        assert_eq!(month_start(june), june);
        assert_eq!(month_start(june + 12_345), june);
        assert_eq!(month_start(june + 29 * DAY + 86_399), june);
        // The second before it is May's.
        assert_eq!(month_start(june - 1), 20_209 * DAY);
        // A leap day belongs to its February; the year ends in December.
        assert_eq!(month_start(19_782 * DAY + 43_200), 19_754 * DAY);
        assert_eq!(month_start(19_754 * DAY - 1), 19_723 * DAY);
        // The epoch itself, and the end of a year.
        assert_eq!(month_start(0), 0);
        assert_eq!(month_start(364 * DAY + 5), 334 * DAY);
        // Every day of a long stretch starts at a day of the month's first.
        for day in 0..20_000u64 {
            let start = month_start(day * DAY + 7);
            assert!(start <= day * DAY && start % DAY == 0, "{day}");
            assert_eq!(month_start(start), start, "{day}");
            assert!(day * DAY - start < 31 * DAY, "{day}");
        }
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
