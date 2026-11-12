//! The friends tables (migration 061): a Discord account's settings, its
//! friendships, requests and blocks, every change a player makes to them,
//! and the reads the friends state is made of (docs/design/FRIENDS.md, "Data
//! model" and "Abuse"). Accounts are Discord user IDs here and nowhere past
//! this module's callers: players name rows only by their random ids.
use ember_protocol::{
    api::ErrorCode,
    friends::{
        BlockedPlayer, CODE_SYMBOLS, FriendRefusal, FriendRoom, MAX_BLOCKS, MAX_FRIENDS,
        MAX_INCOMING, OutgoingRequest, PairRecord,
    },
    levels::Relation,
    rooms::is_player_name,
    skill::Listing,
};
use rusqlite::{OptionalExtension, Row, Transaction, params};

use crate::{
    error::{ApiFailure, Result},
    util::{new_id, random},
};

const DAY_SECS: u64 = 24 * 60 * 60;
/// A request lapses this long after it was sent, hidden or not.
pub const REQUEST_SECS: u64 = 30 * DAY_SECS;
/// New requests one sender makes in a day.
const REQUESTS_A_DAY: u32 = 20;
/// Requests one sender has pending, hidden ones too.
const PENDING: u32 = 30;
/// `seen_at` is written once it is this old, so an online player costs at
/// most one write per this long.
const SEEN_SECS: u64 = 5 * 60;
/// A refusal with its reason, which the client words.
pub fn refuse(reason: FriendRefusal) -> ApiFailure {
    let (code, message) = match reason {
        FriendRefusal::FriendsFull => (ErrorCode::StaleRevision, "That friend list is full."),
        FriendRefusal::RequestLimit => (
            ErrorCode::StaleRevision,
            "You have sent enough requests for now.",
        ),
        FriendRefusal::CodeTooSoon => (
            ErrorCode::StaleRevision,
            "Your friend code changes at most once a day.",
        ),
        FriendRefusal::BlocksFull => (
            ErrorCode::StaleRevision,
            "You have blocked as many players as you can.",
        ),
        FriendRefusal::FriendsOff => (ErrorCode::StaleRevision, "Friends are off."),
        FriendRefusal::OwnCard => (ErrorCode::InvalidRequest, "That is you."),
        FriendRefusal::UnknownCode => {
            (ErrorCode::InvalidRequest, "No player has that friend code.")
        }
        FriendRefusal::CardExpired => (
            ErrorCode::InvalidRequest,
            "That player's card has expired. Meet them in a room again.",
        ),
        FriendRefusal::InviteUnavailable => (
            ErrorCode::StaleRevision,
            "That friend cannot be invited now.",
        ),
        // Written where the account is looked up (`routes::friends::admit`).
        FriendRefusal::NoDiscordAccount => (
            ErrorCode::Forbidden,
            "Connect a Discord account to have friends.",
        ),
    };
    ApiFailure::new(code, message).detail("reason", reason.as_str())
}

/// Two accounts in a pair's stored order: `first` sorts before `second`.
fn ordered<'a>(one: &'a str, other: &'a str) -> (&'a str, &'a str) {
    if one < other {
        (one, other)
    } else {
        (other, one)
    }
}

/// A new friend code: 40 random bits as eight Crockford symbols.
fn random_code() -> String {
    let mut bytes = [0u8; 8];
    bytes[..5].copy_from_slice(&random::<5>());
    let bits = u64::from_le_bytes(bytes);
    let symbols: String = (0..8)
        .map(|at| CODE_SYMBOLS[((bits >> (5 * at)) & 31) as usize] as char)
        .collect();
    format!("{}-{}", &symbols[..4], &symbols[4..])
}

/// A code no account holds.
fn free_code(tx: &Transaction<'_>) -> Result<String> {
    loop {
        let code = random_code();
        let taken: bool = tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM social_accounts WHERE code = ?1)",
            [&code],
            |row| row.get(0),
        )?;
        if !taken {
            return Ok(code);
        }
    }
}

/// What the caller's game sent in a sync. The account is made at its first
/// sync, with a new code; after that only what changed is written, and
/// `seen_at` moves once it is `SEEN_SECS` old or as the game closes
/// (`leaving`), never while the account appears offline. A sync never
/// touches an account that is off, which only `turn_on` brings back: the
/// game syncs by itself, so anything else would turn friends on again.
pub fn sync(
    tx: &Transaction<'_>,
    account: &str,
    listing: &Listing,
    main: Option<u8>,
    leaving: bool,
    now: u64,
) -> Result<()> {
    let off: Option<bool> = tx
        .query_row(
            "SELECT off FROM social_accounts WHERE discord_user_id = ?1",
            [account],
            |row| row.get(0),
        )
        .optional()?;
    if off == Some(true) {
        return Err(refuse(FriendRefusal::FriendsOff));
    }
    if off.is_none() {
        tx.execute(
            "INSERT INTO social_accounts (discord_user_id, name, country, main_fighter, code, code_at, seen_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?6)",
            params![account, listing.name, listing.country, main, free_code(tx)?, now],
        )?;
        return Ok(());
    }
    tx.execute(
        "UPDATE social_accounts SET name = ?2, country = ?3, main_fighter = ?4,
             seen_at = CASE WHEN appear_offline = 0 AND (?5 OR seen_at <= ?6) THEN ?7 ELSE seen_at END
         WHERE discord_user_id = ?1
           AND (name IS NOT ?2 OR country IS NOT ?3 OR main_fighter IS NOT ?4
                OR (appear_offline = 0 AND (?5 OR seen_at <= ?6)))",
        params![
            account,
            listing.name,
            listing.country,
            main,
            leaving,
            now.saturating_sub(SEEN_SECS),
            now
        ],
    )?;
    Ok(())
}

/// An account's friends settings and how friends see it.
pub struct Account {
    pub name: String,
    pub country: Option<String>,
    pub main: Option<u8>,
    pub code: String,
    pub appear_offline: bool,
    pub seen_at: u64,
    /// The player turned friends off (`forget`); only `turn_on` ends it.
    pub off: bool,
}

/// `account`'s settings, once it has synced.
pub fn account(tx: &Transaction<'_>, account: &str) -> Result<Option<Account>> {
    Ok(tx
        .query_row(
            "SELECT name, country, main_fighter, code, appear_offline, seen_at, off
             FROM social_accounts WHERE discord_user_id = ?1",
            [account],
            |row| {
                Ok(Account {
                    name: row.get(0)?,
                    country: row.get(1)?,
                    main: row.get(2)?,
                    code: row.get(3)?,
                    appear_offline: row.get(4)?,
                    seen_at: row.get(5)?,
                    off: row.get(6)?,
                })
            },
        )
        .optional()?)
}

/// The account whose code is `code` (as `parse_friend_code` writes it). An
/// account that is off has none: it reads as one nobody holds.
pub fn account_of_code(tx: &Transaction<'_>, code: &str) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            "SELECT discord_user_id FROM social_accounts WHERE code = ?1 AND off = 0",
            [code],
            |row| row.get(0),
        )
        .optional()?)
}

/// What `caller` is to `target`, from `caller`'s side; None when `target`
/// blocked `caller`, who then sees nothing of them. A request the receiver
/// cannot see does not make them `received`, and one its sender withdrew
/// does not make the sender `sent`.
pub fn relation(tx: &Transaction<'_>, caller: &str, target: &str) -> Result<Option<Relation>> {
    let (first, second) = ordered(caller, target);
    let flags: [bool; 5] = tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM friend_blocks WHERE blocker = ?2 AND blocked = ?1),
                EXISTS (SELECT 1 FROM friend_blocks WHERE blocker = ?1 AND blocked = ?2),
                EXISTS (SELECT 1 FROM friendships WHERE first = ?3 AND second = ?4),
                EXISTS (SELECT 1 FROM friend_requests
                        WHERE sender = ?1 AND receiver = ?2 AND withdrawn_at IS NULL),
                EXISTS (SELECT 1 FROM friend_requests
                        WHERE sender = ?2 AND receiver = ?1 AND hidden_at IS NULL)",
        params![caller, target, first, second],
        |row| Ok([row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?, row.get(4)?]),
    )?;
    Ok(match flags {
        [true, ..] => None,
        [_, true, ..] => Some(Relation::Blocked),
        [_, _, true, ..] => Some(Relation::Friend),
        [_, _, _, true, _] => Some(Relation::Sent),
        [_, _, _, _, true] => Some(Relation::Received),
        _ => Some(Relation::None),
    })
}

fn count(tx: &Transaction<'_>, sql: &str, params: impl rusqlite::Params) -> Result<u32> {
    Ok(tx.query_row(sql, params, |row| row.get(0))?)
}

/// `sender` asks `receiver`, having seen `label`. Nothing changes between
/// friends, after a block by the sender, or while the sender's request
/// stands. When the receiver already asked the sender, it is accepted
/// instead. A new request is hidden from the receiver, and still looks sent,
/// when the receiver blocked the sender or holds `MAX_INCOMING` visible
/// requests; a withdrawn hidden one is the sender's again, as it was.
pub fn ask(
    tx: &Transaction<'_>,
    sender: &str,
    receiver: &str,
    label: &str,
    now: u64,
) -> Result<()> {
    if sender == receiver {
        return Err(refuse(FriendRefusal::OwnCard));
    }
    let relation = relation(tx, sender, receiver)?;
    if matches!(
        relation,
        Some(Relation::Friend | Relation::Blocked | Relation::Sent)
    ) {
        return Ok(());
    }
    let asked: bool = tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM friend_requests
                        WHERE sender = ?1 AND receiver = ?2 AND withdrawn_at IS NULL)",
        params![receiver, sender],
        |row| row.get(0),
    )?;
    if asked {
        return befriend(tx, sender, receiver, now);
    }
    let today = count(
        tx,
        "SELECT COUNT(*) FROM friend_requests WHERE sender = ?1 AND sent_at > ?2",
        params![sender, now.saturating_sub(DAY_SECS)],
    )?;
    let pending = count(
        tx,
        "SELECT COUNT(*) FROM friend_requests WHERE sender = ?1 AND withdrawn_at IS NULL",
        [sender],
    )?;
    if today >= REQUESTS_A_DAY || pending >= PENDING {
        return Err(refuse(FriendRefusal::RequestLimit));
    }
    let inbox = count(
        tx,
        "SELECT COUNT(*) FROM friend_requests WHERE receiver = ?1 AND hidden_at IS NULL",
        [receiver],
    )?;
    // A request to an account that turned friends off is hidden, so it looks
    // sent as one to a player who blocked the sender does, and the sender
    // learns nothing of it. It is never shown when they turn friends on, but
    // like any hidden request it still counts as asked: if they then ask the
    // sender, the two are friends at once, as both asked.
    let off = account(tx, receiver)?.is_some_and(|receiver| receiver.off);
    let hidden = (relation.is_none() || off || inbox as usize >= MAX_INCOMING).then_some(now);
    tx.execute(
        "INSERT INTO friend_requests (id, sender, receiver, label, sent_at, hidden_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6)
         ON CONFLICT (sender, receiver) DO UPDATE SET label = excluded.label,
             sent_at = excluded.sent_at, withdrawn_at = NULL,
             hidden_at = COALESCE(friend_requests.hidden_at, excluded.hidden_at)",
        params![new_id("rq"), sender, receiver, label, now, hidden],
    )?;
    Ok(())
}

/// Makes `one` and `other` friends, unless either has `MAX_FRIENDS`, and
/// drops the requests between them.
fn befriend(tx: &Transaction<'_>, one: &str, other: &str, now: u64) -> Result<()> {
    for account in [one, other] {
        let friends = count(
            tx,
            "SELECT (SELECT COUNT(*) FROM friendships WHERE first = ?1)
                  + (SELECT COUNT(*) FROM friendships WHERE second = ?1)",
            [account],
        )?;
        if friends as usize >= MAX_FRIENDS {
            return Err(refuse(FriendRefusal::FriendsFull));
        }
    }
    tx.execute(
        "DELETE FROM friend_requests WHERE (sender = ?1 AND receiver = ?2) OR (sender = ?2 AND receiver = ?1)",
        params![one, other],
    )?;
    let (first, second) = ordered(one, other);
    tx.execute(
        "INSERT OR IGNORE INTO friendships (first, second, id, since) VALUES (?1, ?2, ?3, ?4)",
        params![first, second, new_id("fr"), now],
    )?;
    Ok(())
}

/// The sender of request `id`, which `receiver` received; `visible` alone
/// asks for one they can see.
fn sender_of(
    tx: &Transaction<'_>,
    receiver: &str,
    id: &str,
    visible: bool,
) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            "SELECT sender FROM friend_requests
             WHERE id = ?1 AND receiver = ?2 AND (hidden_at IS NULL OR NOT ?3)",
            params![id, receiver, visible],
            |row| row.get(0),
        )
        .optional()?)
}

/// `caller` accepts the request `id` they can see.
pub fn accept(tx: &Transaction<'_>, caller: &str, id: &str, now: u64) -> Result<()> {
    match sender_of(tx, caller, id, true)? {
        Some(sender) => befriend(tx, caller, &sender, now),
        None => Ok(()),
    }
}

/// `caller` declines the request `id`: it is hidden from them and stays sent
/// for its sender. With `block`, the sender is blocked too.
pub fn decline(tx: &Transaction<'_>, caller: &str, id: &str, block: bool, now: u64) -> Result<()> {
    let Some(sender) = sender_of(tx, caller, id, false)? else {
        return Ok(());
    };
    tx.execute(
        "UPDATE friend_requests SET hidden_at = COALESCE(hidden_at, ?2) WHERE id = ?1",
        params![id, now],
    )?;
    if block {
        let name = name_of(tx, &sender)?;
        self::block(tx, caller, &sender, &name, now)?;
    }
    Ok(())
}

/// `caller` takes back the request `id`. One its receiver can see is gone;
/// one they cannot is withdrawn, so it holds the pair until it lapses and
/// its sender learns nothing from taking it back.
pub fn cancel(tx: &Transaction<'_>, caller: &str, id: &str, now: u64) -> Result<()> {
    tx.execute(
        "DELETE FROM friend_requests WHERE id = ?1 AND sender = ?2 AND hidden_at IS NULL",
        params![id, caller],
    )?;
    tx.execute(
        "UPDATE friend_requests SET withdrawn_at = COALESCE(withdrawn_at, ?3)
         WHERE id = ?1 AND sender = ?2",
        params![id, caller, now],
    )?;
    Ok(())
}

/// The account `caller` is friends with as `id`.
pub fn friend_of(tx: &Transaction<'_>, caller: &str, id: &str) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            "SELECT CASE WHEN first = ?2 THEN second ELSE first END FROM friendships
             WHERE id = ?1 AND (first = ?2 OR second = ?2)",
            params![id, caller],
            |row| row.get(0),
        )
        .optional()?)
}

/// Ends the friendship `id` of `caller`, and with `block` blocks them too.
/// The account they were friends with, if `id` named one.
pub fn remove(
    tx: &Transaction<'_>,
    caller: &str,
    id: &str,
    block: bool,
    now: u64,
) -> Result<Option<String>> {
    let Some(other) = friend_of(tx, caller, id)? else {
        return Ok(None);
    };
    unfriend(tx, caller, &other)?;
    if block {
        let name = name_of(tx, &other)?;
        self::block(tx, caller, &other, &name, now)?;
    }
    Ok(Some(other))
}

fn unfriend(tx: &Transaction<'_>, one: &str, other: &str) -> Result<()> {
    let (first, second) = ordered(one, other);
    tx.execute(
        "DELETE FROM friendships WHERE first = ?1 AND second = ?2",
        params![first, second],
    )?;
    Ok(())
}

/// The name to list `account` under: its own, or a stand-in for an account
/// that never synced.
fn name_of(tx: &Transaction<'_>, account: &str) -> Result<String> {
    Ok(tx
        .query_row(
            "SELECT name FROM social_accounts WHERE discord_user_id = ?1",
            [account],
            |row| row.get(0),
        )
        .optional()?
        .unwrap_or_else(|| "Player".to_owned()))
}

/// What a sender saw (`check_label`, up to 64 bytes) as a name to list a
/// player under (`is_player_name`, up to 32): cut at a character's edge.
pub fn listed_name(label: &str) -> String {
    let mut end = label.len().min(32);
    while !label.is_char_boundary(end) {
        end -= 1;
    }
    let name = label[..end].trim_end().to_owned();
    debug_assert!(is_player_name(&name), "{name:?}");
    name
}

/// `blocker` blocks `blocked`, listed as `name`: their friendship ends, the
/// blocker's requests to them go, and theirs to the blocker are hidden, so
/// they still look sent. Blocking twice changes nothing.
pub fn block(
    tx: &Transaction<'_>,
    blocker: &str,
    blocked: &str,
    name: &str,
    now: u64,
) -> Result<()> {
    if blocker == blocked {
        return Err(refuse(FriendRefusal::OwnCard));
    }
    let already: bool = tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM friend_blocks WHERE blocker = ?1 AND blocked = ?2)",
        params![blocker, blocked],
        |row| row.get(0),
    )?;
    if already {
        return Ok(());
    }
    let blocks = count(
        tx,
        "SELECT COUNT(*) FROM friend_blocks WHERE blocker = ?1",
        [blocker],
    )?;
    if blocks as usize >= MAX_BLOCKS {
        return Err(refuse(FriendRefusal::BlocksFull));
    }
    unfriend(tx, blocker, blocked)?;
    tx.execute(
        "DELETE FROM friend_requests WHERE sender = ?1 AND receiver = ?2",
        params![blocker, blocked],
    )?;
    tx.execute(
        "UPDATE friend_requests SET hidden_at = COALESCE(hidden_at, ?3)
         WHERE sender = ?1 AND receiver = ?2",
        params![blocked, blocker, now],
    )?;
    tx.execute(
        "INSERT INTO friend_blocks (blocker, blocked, id, name, at) VALUES (?1, ?2, ?3, ?4, ?5)",
        params![blocker, blocked, new_id("bl"), name, now],
    )?;
    Ok(())
}

/// Lifts `caller`'s block `id`. Requests it hid stay hidden until they lapse.
pub fn unblock(tx: &Transaction<'_>, caller: &str, id: &str) -> Result<()> {
    tx.execute(
        "DELETE FROM friend_blocks WHERE id = ?1 AND blocker = ?2",
        params![id, caller],
    )?;
    Ok(())
}

/// Friends see `caller` as offline, from now, or no longer. `seen_at` is
/// set as it is turned on and then frozen (`sync`).
pub fn appear_offline(tx: &Transaction<'_>, caller: &str, on: bool, now: u64) -> Result<()> {
    tx.execute(
        "UPDATE social_accounts
         SET seen_at = CASE WHEN ?2 AND appear_offline = 0 THEN ?3 ELSE seen_at END,
             appear_offline = ?2
         WHERE discord_user_id = ?1",
        params![caller, on, now],
    )?;
    Ok(())
}

/// A new code for `caller`, at most once a day; the old one stops working.
pub fn new_code(tx: &Transaction<'_>, caller: &str, now: u64) -> Result<()> {
    let code_at: u64 = tx.query_row(
        "SELECT code_at FROM social_accounts WHERE discord_user_id = ?1",
        [caller],
        |row| row.get(0),
    )?;
    let wait = (code_at + DAY_SECS).saturating_sub(now);
    if wait > 0 {
        return Err(refuse(FriendRefusal::CodeTooSoon).detail("change_in", wait));
    }
    tx.execute(
        "UPDATE social_accounts SET code = ?2, code_at = ?3 WHERE discord_user_id = ?1",
        params![caller, free_code(tx)?, now],
    )?;
    Ok(())
}

/// Turns `caller`'s friends off: their friendships and requests both ways
/// are deleted and their code is replaced, so the old one stops working, and
/// the account is stored as off (`Account::off`) until `turn_on`. Their own
/// blocks stay, since they protect the player and are not friends' data;
/// blocks others placed on them are those players' own. The row, with the
/// name, flag and main its game last sent, stays too, so turning friends on
/// needs nothing from the game but the action.
pub fn forget(tx: &Transaction<'_>, caller: &str, now: u64) -> Result<()> {
    for statement in [
        "DELETE FROM friendships WHERE first = ?1",
        "DELETE FROM friendships WHERE second = ?1",
        "DELETE FROM friend_requests WHERE sender = ?1",
        "DELETE FROM friend_requests WHERE receiver = ?1",
    ] {
        tx.execute(statement, [caller])?;
    }
    tx.execute(
        "UPDATE social_accounts SET off = 1, code = ?2, code_at = ?3 WHERE discord_user_id = ?1",
        params![caller, free_code(tx)?, now],
    )?;
    Ok(())
}

/// Turns `caller`'s friends on again after `forget`: no friends or requests,
/// the new code, and the blocks they kept. Nothing for an account that is on.
pub fn turn_on(tx: &Transaction<'_>, caller: &str, now: u64) -> Result<()> {
    tx.execute(
        "UPDATE social_accounts SET off = 0, seen_at = ?2 WHERE discord_user_id = ?1 AND off = 1",
        params![caller, now],
    )?;
    Ok(())
}

/// Requests lapse `REQUEST_SECS` after they were sent, hidden or not.
pub fn sweep(tx: &Transaction<'_>, now: u64) -> Result<()> {
    tx.execute(
        "DELETE FROM friend_requests WHERE sent_at <= ?1",
        [now.saturating_sub(REQUEST_SECS)],
    )?;
    Ok(())
}

/// One of an account's friends, as stored.
pub struct FriendRow {
    pub id: String,
    pub since: u64,
    pub account: String,
    pub shown: Account,
}

/// `caller`'s friends, at most `MAX_FRIENDS`.
pub fn friends(tx: &Transaction<'_>, caller: &str) -> Result<Vec<FriendRow>> {
    let mut statement = tx.prepare(
        "SELECT f.id, f.since, f.other, s.name, s.country, s.main_fighter, s.code,
                s.appear_offline, s.seen_at, s.off
         FROM (SELECT id, since, second AS other FROM friendships WHERE first = ?1
               UNION ALL
               SELECT id, since, first FROM friendships WHERE second = ?1) f
         JOIN social_accounts s ON s.discord_user_id = f.other
         LIMIT ?2",
    )?;
    let rows = statement
        .query_map(params![caller, MAX_FRIENDS as i64], |row| {
            Ok(FriendRow {
                id: row.get(0)?,
                since: row.get(1)?,
                account: row.get(2)?,
                shown: Account {
                    name: row.get(3)?,
                    country: row.get(4)?,
                    main: row.get(5)?,
                    code: row.get(6)?,
                    appear_offline: row.get(7)?,
                    seen_at: row.get(8)?,
                    off: row.get(9)?,
                },
            })
        })?
        .collect::<rusqlite::Result<_>>()?;
    Ok(rows)
}

/// A request `caller` can see, with its sender.
pub struct IncomingRow {
    pub id: String,
    pub sender: String,
    pub name: String,
    pub country: Option<String>,
    pub sent_at: u64,
}

/// The requests `caller` can see, newest first, at most `MAX_INCOMING`.
pub fn incoming(tx: &Transaction<'_>, caller: &str) -> Result<Vec<IncomingRow>> {
    let mut statement = tx.prepare(
        "SELECT r.id, r.sender, s.name, s.country, r.sent_at
         FROM friend_requests r JOIN social_accounts s ON s.discord_user_id = r.sender
         WHERE r.receiver = ?1 AND r.hidden_at IS NULL
         ORDER BY r.sent_at DESC LIMIT ?2",
    )?;
    let rows = statement
        .query_map(params![caller, MAX_INCOMING as i64], |row| {
            Ok(IncomingRow {
                id: row.get(0)?,
                sender: row.get(1)?,
                name: row.get(2)?,
                country: row.get(3)?,
                sent_at: row.get(4)?,
            })
        })?
        .collect::<rusqlite::Result<_>>()?;
    Ok(rows)
}

/// The requests `caller` sent and did not withdraw, hidden ones too: they
/// stay "sent". Newest first.
pub fn outgoing(tx: &Transaction<'_>, caller: &str) -> Result<Vec<OutgoingRequest>> {
    let mut statement = tx.prepare(
        "SELECT id, label, sent_at FROM friend_requests
         WHERE sender = ?1 AND withdrawn_at IS NULL ORDER BY sent_at DESC",
    )?;
    let rows = statement
        .query_map([caller], |row| {
            Ok(OutgoingRequest {
                id: row.get(0)?,
                label: row.get(1)?,
                sent_at: row.get(2)?,
            })
        })?
        .collect::<rusqlite::Result<_>>()?;
    Ok(rows)
}

/// The players `caller` blocked, newest first, at most `MAX_BLOCKS`.
pub fn blocked(tx: &Transaction<'_>, caller: &str) -> Result<Vec<BlockedPlayer>> {
    let mut statement = tx.prepare(
        "SELECT id, name, at FROM friend_blocks WHERE blocker = ?1 ORDER BY at DESC LIMIT ?2",
    )?;
    let rows = statement
        .query_map(params![caller, MAX_BLOCKS as i64], |row| {
            Ok(BlockedPlayer {
                id: row.get(0)?,
                name: row.get(1)?,
                at: row.get(2)?,
            })
        })?
        .collect::<rusqlite::Result<_>>()?;
    Ok(rows)
}

/// The record between `caller` and `other` from `caller`'s side
/// (`pair_records`, migration 060): absent before their first counted game.
pub fn record(tx: &Transaction<'_>, caller: &str, other: &str) -> Result<Option<PairRecord>> {
    let (first, second) = ordered(caller, other);
    let stored: Option<(u64, u64, u64)> = tx
        .query_row(
            "SELECT first_wins, second_wins, played_at FROM pair_records
             WHERE first = ?1 AND second = ?2",
            params![first, second],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
        )
        .optional()?;
    Ok(stored
        .map(|(first_wins, second_wins, last_at)| {
            let (won, lost) = if caller == first {
                (first_wins, second_wins)
            } else {
                (second_wins, first_wins)
            };
            PairRecord { won, lost, last_at }
        })
        .filter(|record| record.won + record.lost > 0))
}

/// The public room `room_id` as a friend's presence shows it, while it is
/// open and listed (the filter of `rooms::player_rooms`): joinable when it is
/// not locked, not full and of `build_id`. None for any other room.
pub fn listed_room(
    tx: &Transaction<'_>,
    room_id: &str,
    build_id: Option<&str>,
) -> Result<Option<FriendRoom>> {
    let room = tx
        .query_row(
            "SELECT name, members, capacity, build_id = ?2, COALESCE(locked, 0) FROM rooms
             WHERE room_id = ?1 AND closed_at IS NULL AND invitation_sealed IS NOT NULL
               AND region IS NOT NULL AND members >= 1",
            params![room_id, build_id],
            |row| room_of(room_id, row),
        )
        .optional()?;
    Ok(room.filter(|room| room.check().is_ok()))
}

fn room_of(room_id: &str, row: &Row<'_>) -> rusqlite::Result<FriendRoom> {
    let (members, capacity): (u8, u8) = (row.get(1)?, row.get(2)?);
    let (same_build, locked): (Option<bool>, bool) = (row.get(3)?, row.get(4)?);
    Ok(FriendRoom {
        room_id: room_id.to_owned(),
        name: row.get(0)?,
        members,
        capacity,
        joinable: same_build == Some(true) && !locked && members < capacity,
    })
}

#[cfg(test)]
mod tests {
    use ember_protocol::friends::{is_friend_code, parse_friend_code};

    use super::*;

    #[test]
    fn a_random_code_is_a_friend_code_and_reads_back_as_itself() {
        for _ in 0..1_000 {
            let code = random_code();
            assert!(is_friend_code(&code), "{code}");
            assert_eq!(parse_friend_code(&code.to_lowercase()), Some(code));
        }
    }

    #[test]
    fn a_label_becomes_a_name_to_list_under() {
        assert_eq!(listed_name("Player 1"), "Player 1");
        let long = format!("{}  {}", "a".repeat(30), "b".repeat(30));
        assert_eq!(listed_name(&long), "a".repeat(30));
        // Never cut inside a character.
        let wide = "\u{e9}".repeat(20);
        assert_eq!(listed_name(&wide), "\u{e9}".repeat(16));
        assert!(is_player_name(&listed_name(&"\u{1f600}".repeat(16))));
    }

    #[test]
    fn a_pair_is_stored_in_order() {
        assert_eq!(ordered("200", "100"), ("100", "200"));
        assert_eq!(ordered("100", "200"), ("100", "200"));
    }
}
