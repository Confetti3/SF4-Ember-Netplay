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
use std::collections::HashMap;

use ember_protocol::{
    encoding::{b64u, decode_b64u},
    friends::PairRecord,
    levels::{CARD_PREFIX, CARD_VALID_SECS, CardSave, PlayerCard, Relation, Style, Worn},
    rooms::is_default_game,
    skill::{Listing, SkillBadge},
};
use rusqlite::{OptionalExtension, Row, Transaction, params};

use crate::{
    AppState,
    db::select_in,
    error::Result,
    friends::{self, Classes, store},
    routes::rooms,
    secrets::Keys,
    skill::{self, levels},
};

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

/// Whether a card carries its player's friend code
/// (`levels::CARD_FRIEND_CODE_FEATURE`, docs/design/PROFILE.md, "The friend
/// code on a card"): where public rooms are on, friends are not offered and
/// the bridge's game is not USF4, so its players keep their friends on
/// another bridge and add a room member there by the code their card here
/// shows. The one place the switch is read: discovery, the card save and the
/// look-up all ask this.
pub fn codes_shown(state: &AppState) -> bool {
    rooms::enabled(state) && !friends::offered(state) && !is_default_game(&rooms::game(state))
}

/// A save's friend code, which only a bridge whose cards show codes takes: a
/// client names one only to a bridge that lists the capability, so one sent
/// elsewhere is a client's mistake.
pub fn check_friend_code(state: &AppState, card: &CardSave) -> Result<()> {
    if card.friend_code.is_some() && !codes_shown(state) {
        return Err(ember_protocol::Error::InvalidField("friend_code").into());
    }
    Ok(())
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
    /// The friend code its last save carried, which a card shows only while
    /// `codes_shown`.
    pub friend_code: Option<String>,
}

/// The `player_cards` columns a `Stored` is read from, in its order.
const STORED: &str = "name, country, main_fighter, since, worn_title, worn_icon, pins,
                      device, link, region, looking, friend_code";

/// The `Stored` in columns `at` onwards of a row that selected `STORED`.
fn stored_in(row: &Row<'_>, at: usize) -> rusqlite::Result<Stored> {
    let pins: String = row.get(at + 6)?;
    Ok(Stored {
        name: row.get(at)?,
        country: row.get(at + 1)?,
        main: row.get(at + 2)?,
        since: row.get(at + 3)?,
        worn: Worn {
            title: row.get(at + 4)?,
            icon: row.get(at + 5)?,
        },
        pins: pins
            .split(',')
            .filter(|pin| !pin.is_empty())
            .map(str::to_owned)
            .collect(),
        style: Style {
            device: row.get(at + 7)?,
            link: row.get(at + 8)?,
            region: row.get(at + 9)?,
            looking: row.get(at + 10)?,
        },
        friend_code: row.get(at + 11)?,
    })
}

/// The rows of those of `accounts` that have one and a Discord account
/// connected, in one read.
fn listings(tx: &Transaction<'_>, accounts: &[&str]) -> Result<HashMap<String, Stored>> {
    Ok(select_in(
        tx,
        &format!(
            "SELECT discord_user_id, {STORED} FROM player_cards
             WHERE discord_user_id IN ({{ids}})
               AND EXISTS (SELECT 1 FROM discord_accounts d
                           WHERE d.user_id = player_cards.discord_user_id)"
        ),
        &[],
        accounts,
        |row| Ok((row.get::<_, String>(0)?, stored_in(row, 1)?)),
    )?
    .into_iter()
    .collect())
}

/// Replaces `account`'s card with what its player chose to show, its friend
/// code with the rest (a save without one clears it; `routes::profiles::save`
/// takes one only while `codes_shown`). The row is made here the first time,
/// from when the account connected; `since` is never moved after.
pub fn save(tx: &Transaction<'_>, account: &str, card: &CardSave, now: u64) -> Result<()> {
    tx.execute(
        "INSERT INTO player_cards (discord_user_id, name, country, main_fighter, since,
             worn_title, worn_icon, pins, device, link, region, looking, friend_code, updated_at)
         VALUES (?1, ?2, ?3, ?4,
             COALESCE((SELECT connected_at FROM discord_accounts WHERE user_id = ?1), ?5),
             ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?5)
         ON CONFLICT (discord_user_id) DO UPDATE SET name = ?2, country = ?3, main_fighter = ?4,
             worn_title = ?6, worn_icon = ?7, pins = ?8, device = ?9, link = ?10, region = ?11,
             looking = ?12, friend_code = ?13, updated_at = ?5",
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
            card.style.stored_region(),
            card.style.looking,
            card.friend_code,
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
/// month they came, the families they wear and show, how they play, and,
/// while `codes_shown`, their friend code.
fn describe(card: PlayerCard, stored: &Stored, codes_shown: bool) -> PlayerCard {
    PlayerCard {
        name: Some(stored.name.clone()),
        country: stored.country.clone(),
        main: stored.main,
        since: Some(month_start(stored.since)),
        worn: stored.worn.clone(),
        pins: stored.pins.clone(),
        style: stored.style.clone(),
        friend_code: stored.friend_code.clone().filter(|_| codes_shown),
        ..card
    }
}

/// Who reads cards, and how the bridge shows them: the caller's Discord
/// account, whether friends are offered, whether cards show friend codes
/// (`codes_shown`), the classes, and the time. A block is between Discord
/// accounts, so only a caller with one reads another player's card
/// (`routes::levels::lookup`, docs/design/PROFILE.md, "Privacy").
pub struct Viewer<'a> {
    pub caller: &'a str,
    pub friends_offered: bool,
    pub codes_shown: bool,
    pub classes: &'a Classes,
    pub now: u64,
}

/// What a `Reader` is read for, which decides what it reads beyond the
/// caller's relation to each account and, while the bridge shows them, the
/// classes. Each read is a fixed number of statements, whatever the number of
/// accounts.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Purpose {
    /// A card look-up: everything a card says.
    Card,
    /// A friends state's friends and requests: points and records.
    Friends,
    /// A search's hits: nothing more.
    Hits,
    /// The most played opponents: their names.
    Rivals,
}

impl Purpose {
    /// The points and fighters (`levels::cards`).
    fn points(self) -> bool {
        matches!(self, Self::Card | Self::Friends)
    }

    /// The award counters a card shows, read with the points (so only a
    /// purpose that reads the points may answer true).
    fn awards(self) -> bool {
        self == Self::Card
    }

    /// The `player_cards` rows.
    fn listings(self) -> bool {
        matches!(self, Self::Card | Self::Rivals)
    }

    /// The caller's records against each.
    fn records(self) -> bool {
        matches!(self, Self::Card | Self::Friends)
    }
}

/// What the bridge holds of a set of accounts for a viewer, read at once:
/// the one place a card is assembled, and what a search hit, the most played
/// opponent and a friends state show of other players. It costs the same few
/// reads (`Purpose`) whatever the number of accounts, all in the one
/// transaction it is read in. What a purpose does not read reads as none, so
/// each caller asks only for what its purpose reads (`reader_tests` holds,
/// per purpose, the accessors that answer).
pub struct Reader {
    caller: String,
    friends_offered: bool,
    codes_shown: bool,
    cards: HashMap<String, PlayerCard>,
    badges: HashMap<String, SkillBadge>,
    listings: HashMap<String, Stored>,
    relations: store::Relations,
    records: HashMap<String, PairRecord>,
}

impl Reader {
    /// Reads `accounts` for `viewer` and `purpose`. An account may come more
    /// than once; it is read once.
    pub fn read(
        tx: &Transaction<'_>,
        viewer: &Viewer<'_>,
        accounts: &[&str],
        purpose: Purpose,
    ) -> Result<Self> {
        let mut accounts = accounts.to_vec();
        accounts.sort_unstable();
        accounts.dedup();
        let classes = viewer.classes;
        let awards = purpose.awards();
        let standings = if classes.shown || awards {
            Some(skill::Standings::read(
                tx,
                &classes.game,
                &accounts,
                viewer.now,
            )?)
        } else {
            None
        };
        let cards = if purpose.points() {
            levels::cards(tx, &accounts, standings.as_ref().filter(|_| awards))?
        } else {
            HashMap::new()
        };
        let badges = match &standings {
            Some(standings) if classes.shown => accounts
                .iter()
                .filter_map(|account| {
                    let badge = standings.last_shown(account, &classes.developers)?;
                    Some(((*account).to_owned(), badge))
                })
                .collect(),
            _ => HashMap::new(),
        };
        let listings = if purpose.listings() {
            listings(tx, &accounts)?
        } else {
            HashMap::new()
        };
        let records = if purpose.records() {
            store::records(tx, viewer.caller, &accounts)?
        } else {
            HashMap::new()
        };
        Ok(Self {
            caller: viewer.caller.to_owned(),
            friends_offered: viewer.friends_offered,
            codes_shown: viewer.codes_shown,
            cards,
            badges,
            listings,
            relations: store::relations(tx, viewer.caller, &accounts)?,
            records,
        })
    }

    /// `target`'s card as the caller sees it: none when `target` blocked the
    /// caller or has no Discord account connected; otherwise their points,
    /// the counters behind their awards, what they chose to show (with their
    /// friend code while cards show codes), their class while classes are
    /// shown and, for a caller who is not themselves, their record against
    /// the caller. Their relation, and the id of the friendship when it is
    /// one, come once friends are offered. Read for `Purpose::Card`.
    pub fn card(&self, target: &str) -> Option<PlayerCard> {
        let relation = self.relation(target)?;
        let card = self.cards.get(target)?.clone();
        let relation = if self.friends_offered {
            relation
        } else {
            Relation::None
        };
        let mut card = PlayerCard {
            relation,
            badge: self.badge(target),
            ..card
        };
        if let Some(stored) = self.listings.get(target) {
            card = describe(card, stored, self.codes_shown);
        }
        if self.caller != target {
            card.versus = self.record(target);
            if relation == Relation::Friend {
                card.friend = self.relations.friendship(target);
            }
        }
        Some(card)
    }

    /// The caller's relation to `account` (`store::Relations`): none when it
    /// blocked the caller.
    pub fn relation(&self, account: &str) -> Option<Relation> {
        self.relations.relation(account)
    }

    /// `account`'s points and fighters as its card shows them, when it has a
    /// Discord account connected.
    pub fn points(&self, account: &str) -> Option<&PlayerCard> {
        self.cards.get(account)
    }

    /// The class `account` shows now, as a friends state shows it: none while
    /// the bridge hides classes or when the player does not take part.
    pub fn badge(&self, account: &str) -> Option<SkillBadge> {
        self.badges.get(account).copied()
    }

    /// `account`'s `player_cards` row, while a Discord account is connected.
    pub fn listing(&self, account: &str) -> Option<&Stored> {
        self.listings.get(account)
    }

    /// The caller's record against `account`, from the caller's side.
    pub fn record(&self, account: &str) -> Option<PairRecord> {
        self.records.get(account).copied()
    }
}

#[cfg(test)]
#[path = "cards/reader_tests.rs"]
mod reader_tests;

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
/// begin with it, each most recently updated first. An account that turned
/// friends off (`social_accounts.off`, `friends::store::forget`) is not
/// found: turning friends off is how a player leaves search, as it is how
/// their friend code stops naming them. A caller drops the ones it may not
/// list and keeps what it wants.
pub fn find_by_name(tx: &Transaction<'_>, name: &str, scan: usize) -> Result<Vec<Found>> {
    let pattern = format!(
        "{}%",
        name.replace('\\', "\\\\")
            .replace('%', "\\%")
            .replace('_', "\\_")
    );
    let found = tx
        .prepare_cached(
            "SELECT discord_user_id, name, country, main_fighter, since FROM player_cards
             WHERE name LIKE ?1 ESCAPE '\\'
               AND EXISTS (SELECT 1 FROM discord_accounts d WHERE d.user_id = player_cards.discord_user_id)
               AND NOT EXISTS (SELECT 1 FROM social_accounts s
                               WHERE s.discord_user_id = player_cards.discord_user_id AND s.off = 1)
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
            assert!(start <= day * DAY && start.is_multiple_of(DAY), "{day}");
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
