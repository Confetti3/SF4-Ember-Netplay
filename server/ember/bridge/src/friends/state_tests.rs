//! The friends state of a player with many friends: it is what building each
//! friend on its own makes, in the same order, and it costs the same
//! number of statements for 5 friends as for 150.
use std::{cell::Cell, path::Path, sync::Arc};

use ember_protocol::{
    friends::{PresenceReport, RoomReport, RoomStatus},
    skill::SkillBadge,
};
use rusqlite::{
    OptionalExtension, Transaction, params,
    trace::{TraceEvent, TraceEventCodes},
};

use super::*;
use crate::Db;

const NOW: u64 = 1_800_000_000;
const CALLER: &str = "e000";
const BUILD: &str = "build-1";
/// Listed rooms that friends are in, with whether each is open and of the
/// caller's build: the open one of the same build is joinable.
const ROOMS: [(&str, bool, bool); 4] = [
    ("00000000000000000000000000000001", true, true),
    ("00000000000000000000000000000002", true, false),
    ("00000000000000000000000000000003", false, true),
    ("00000000000000000000000000000004", true, true),
];

thread_local! {
    static STATEMENTS: Cell<usize> = const { Cell::new(0) };
}

fn count_statement(event: TraceEvent<'_>) {
    if matches!(event, TraceEvent::Stmt(..)) {
        STATEMENTS.with(|statements| statements.set(statements.get() + 1));
    }
}

/// What `work` returns, and the statements it ran on `tx`'s connection.
fn counted<T>(tx: &Transaction<'_>, work: impl FnOnce() -> T) -> (T, usize) {
    STATEMENTS.with(|statements| statements.set(0));
    tx.trace_v2(TraceEventCodes::SQLITE_TRACE_STMT, Some(count_statement));
    let value = work();
    tx.trace_v2(TraceEventCodes::SQLITE_TRACE_STMT, None);
    (value, STATEMENTS.with(Cell::get))
}

fn uuid(prefix: &str, number: usize) -> String {
    format!("{prefix}_{number:08x}-1111-4111-8111-111111111111")
}

/// A friend's Discord account, on either side of the caller in sort order.
fn friend_account(number: usize) -> String {
    format!("{}{number:03}", if number % 2 == 0 { 'd' } else { 'f' })
}

fn social(
    tx: &Transaction<'_>,
    account: &str,
    number: usize,
    off_line: bool,
) -> rusqlite::Result<()> {
    tx.execute(
        "INSERT INTO social_accounts
             (discord_user_id, name, country, main_fighter, code, code_at, appear_offline, seen_at)
         VALUES (?1, ?2, ?3, ?4, ?5, 0, ?6, ?7)",
        params![
            account,
            format!("Player {number}"),
            (number % 2 == 0).then_some("JP"),
            (number % 5 != 0).then_some((number % 40) as u8),
            format!("code-{account}"),
            off_line,
            NOW - 1_000 - number as u64,
        ],
    )?;
    Ok(())
}

/// The caller with `friends` friends, and a few of what a friend can be: not
/// connected to Discord, never played, played, opted out of ranking, rated
/// and settled or still placing, a developer, in a room and offline.
fn seed(tx: &Transaction<'_>, friends: usize, now: u64) -> rusqlite::Result<()> {
    social(tx, CALLER, 0, false)?;
    tx.execute(
        "UPDATE social_accounts SET code = 'ABCD-EFGH' WHERE discord_user_id = ?1",
        [CALLER],
    )?;
    for (at, (room_id, open, same_build)) in ROOMS.iter().enumerate() {
        tx.execute(
            "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at,
                                closed_at, members, invitation_sealed, region)
             VALUES (?1, ?2, 8, ?3, ?4, 1, ?5, 2, x'00', 'eu')",
            params![
                room_id,
                format!("Room {at}"),
                if *same_build { BUILD } else { "build-2" },
                format!("creator-{at}"),
                (!open).then_some(2),
            ],
        )?;
    }
    for number in 1..=friends {
        let n = number as u64;
        let account = friend_account(number);
        social(tx, &account, number, number % 11 == 0)?;
        let (first, second) = if account.as_str() < CALLER {
            (account.as_str(), CALLER)
        } else {
            (CALLER, account.as_str())
        };
        tx.execute(
            "INSERT INTO friendships (first, second, id, since) VALUES (?1, ?2, ?3, ?4)",
            params![first, second, uuid("fr", number), n],
        )?;
        if number % 4 != 0 {
            let ember_id = format!("ember-{number}");
            tx.execute(
                "INSERT INTO identities (ember_id, public_key, created_at) VALUES (?1, ?1, 1)",
                [&ember_id],
            )?;
            tx.execute(
                "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at)
                 VALUES (?1, ?2, ?2, 1)",
                params![account, ember_id],
            )?;
        }
        if number % 3 != 0 {
            tx.execute(
                "INSERT INTO account_points (discord_user_id, games, wins, lower_wins, first_at, played_at)
                 VALUES (?1, ?2, ?3, ?4, 1, 1)",
                params![account, n + 1, n / 2, n / 4],
            )?;
            for fighter in [n % 40, (n + 1) % 40] {
                tx.execute(
                    "INSERT INTO fighter_levels (discord_user_id, fighter, games, wins, lower_wins, first_at, played_at)
                     VALUES (?1, ?2, ?3, ?4, ?5, 1, 1)",
                    params![account, fighter, n * 7, n * 3, n],
                )?;
            }
        }
        if number % 7 == 0 {
            tx.execute(
                "INSERT INTO ranking_choices (game, discord_user_id, ranked) VALUES ('usf4', ?1, 0)",
                [&account],
            )?;
        }
        if number % 2 == 0 {
            tx.execute(
                "INSERT INTO skill_ratings (game, discord_user_id, rating, deviation, volatility, band, played_at)
                 VALUES ('usf4', ?1, ?2, ?3, 0.06, ?4, ?5)",
                params![
                    account,
                    1_300.0 + n as f64 * 4.0,
                    if number % 4 == 0 { 80.0 } else { 320.0 },
                    n % 10 + 1,
                    now - n * 3_600,
                ],
            )?;
        }
        // A counted game, or none, as the pair's first account sees it.
        if number % 3 != 1 {
            tx.execute(
                "INSERT INTO pair_records (first, second, first_wins, second_wins, played_at)
                 VALUES (?1, ?2, ?3, ?4, ?5)",
                params![first, second, n % 5, n % 3, n],
            )?;
        }
    }
    // Requests the caller can see, and a sender who also played the caller.
    for number in 0..5 {
        let sender = format!("g{number:03}");
        social(tx, &sender, 900 + number, false)?;
        tx.execute(
            "INSERT INTO friend_requests (id, sender, receiver, label, sent_at)
             VALUES (?1, ?2, ?3, 'label', ?4)",
            params![uuid("rq", number), sender, CALLER, 100 + number as u64],
        )?;
        if number % 2 == 0 {
            tx.execute(
                "INSERT INTO pair_records (first, second, first_wins, second_wins, played_at)
                 VALUES (?1, ?2, 3, 1, 5)",
                params![CALLER, sender],
            )?;
        }
    }
    tx.execute(
        "INSERT INTO friend_requests (id, sender, receiver, label, sent_at)
         VALUES (?1, ?2, ?3, 'label', 50)",
        params![uuid("rq", 99), CALLER, "d001"],
    )?;
    tx.execute(
        "INSERT INTO friend_blocks (blocker, blocked, id, name, at) VALUES (?1, 'h000', ?2, 'Blocked', 9)",
        params![CALLER, uuid("bl", 1)],
    )?;
    Ok(())
}

/// What each friend is doing: most online in the menus, some in a room, some
/// not heard from for a while, some never.
fn report(live: &Live, friends: usize, now: u64) {
    live.report(CALLER, PresenceReport::Menus {}, BUILD.into(), now);
    for number in 1..=friends {
        let account = friend_account(number);
        let room = |room_id: Option<&str>| PresenceReport::Room {
            room: RoomReport {
                room_id: room_id.map(str::to_owned),
                private: room_id.is_none(),
                status: RoomStatus::Fighting,
                table: Some(1),
            },
        };
        let (presence, at) = match number % 6 {
            0 => (PresenceReport::OfflinePlay {}, now - 10),
            1 => (PresenceReport::Menus {}, now - 10),
            2 => (room(Some(ROOMS[number / 2 % ROOMS.len()].0)), now - 10),
            3 => (room(None), now - 10),
            4 => (PresenceReport::Menus {}, now - 500),
            _ => continue,
        };
        live.report(&account, presence, BUILD.into(), at);
    }
}

/// The state as it was built before: one friend at a time, a few reads each.
fn one_at_a_time(
    tx: &Transaction<'_>,
    live: &Live,
    caller: &str,
    classes: &Classes,
    now: u64,
) -> Result<FriendsState> {
    let own = store::account(tx, caller)?.ok_or_else(resync)?;
    let build = live.seen(caller).map(|seen| seen.build_id);
    let mut friends = Vec::new();
    for row in store::friends(tx, caller)? {
        let room = |room_id: &str| -> Result<Option<FriendRoom>> {
            let room = tx
                .query_row(
                    "SELECT room_id, name, members, capacity, build_id = ?2, COALESCE(locked, 0) FROM rooms
                     WHERE room_id = ?1 AND closed_at IS NULL AND invitation_sealed IS NOT NULL
                       AND region IS NOT NULL AND members >= 1",
                    params![room_id, build],
                    |row| {
                        let (members, capacity): (u8, u8) = (row.get(2)?, row.get(3)?);
                        let (same_build, locked): (Option<bool>, bool) = (row.get(4)?, row.get(5)?);
                        Ok(FriendRoom {
                            room_id: row.get(0)?,
                            name: row.get(1)?,
                            members,
                            capacity,
                            joinable: same_build == Some(true) && !locked && members < capacity,
                        })
                    },
                )
                .optional()?;
            Ok(room.filter(|room| room.check().is_ok()))
        };
        let seen = visible(live, &row, now);
        let listed = match seen.as_ref().map(|seen| &seen.doing) {
            Some(Doing::Room(report)) => match &report.room_id {
                Some(room_id) => room(room_id.as_str())?
                    .map(|room| HashMap::from([(room_id.clone(), room)]))
                    .unwrap_or_default(),
                None => HashMap::new(),
            },
            _ => HashMap::new(),
        };
        let (main, account) = levels_of(&row, skill::levels::card(tx, &row.account)?);
        let badge = if classes.shown {
            skill::last_shown(tx, &row.account, &classes.developers, now)?
        } else {
            None
        };
        friends.push(Friend {
            id: row.id.clone(),
            since: row.since,
            main,
            account,
            badge,
            presence: presence_of(&row, seen, &listed),
            record: store::record(tx, caller, &row.account)?,
            name: row.shown.name,
            country: row.shown.country,
        });
    }
    let mut incoming = Vec::new();
    for row in store::incoming(tx, caller)? {
        incoming.push(IncomingRequest {
            played: store::record(tx, caller, &row.sender)?.map(|record| Played {
                games: record.won + record.lost,
                last_at: record.last_at,
            }),
            id: row.id,
            name: row.name,
            country: row.country,
            sent_at: row.sent_at,
        });
    }
    Ok(FriendsState {
        code: own.code,
        appear_offline: own.appear_offline,
        friends,
        incoming,
        outgoing: store::outgoing(tx, caller)?,
        invites: Vec::new(),
        blocked: store::blocked(tx, caller)?,
    })
}

/// Both builds of the state of a caller with `friends` friends, and the
/// statements the new one ran.
async fn both(friends: usize) -> (FriendsState, FriendsState, usize) {
    let db = Db::open(Path::new(":memory:")).unwrap();
    let live = Arc::new(Live::default());
    db.write(move |tx| Ok(seed(tx, friends, NOW)?))
        .await
        .unwrap();
    report(&live, friends, NOW);
    let classes = Classes {
        shown: true,
        developers: vec![friend_account(4), friend_account(10)],
    };
    db.read(move |tx| {
        let (built, statements) = counted(tx, || state(tx, &live, CALLER, &classes, NOW));
        let expected = one_at_a_time(tx, &live, CALLER, &classes, NOW)?;
        Ok((built?, expected, statements))
    })
    .await
    .unwrap()
}

#[tokio::test]
async fn a_state_of_many_friends_is_what_one_friend_at_a_time_builds_in_the_same_order() {
    let (state, expected, _) = both(150).await;
    assert_eq!(state.friends.len(), 150);
    assert_eq!(state, expected);
    assert_eq!(
        serde_json::to_string(&state).unwrap(),
        serde_json::to_string(&expected).unwrap()
    );
    // Each kind of friend is among them, so the equality means something.
    let friends = &state.friends;
    assert!(friends.iter().any(|friend| friend.account.is_none()));
    assert!(friends.iter().any(|friend| friend.account.is_some()));
    assert!(friends.iter().any(|friend| friend.badge.is_none()));
    assert!(
        friends
            .iter()
            .any(|friend| friend.badge == Some(SkillBadge::DEVELOPER))
    );
    assert!(friends.iter().any(|friend| {
        friend
            .badge
            .as_ref()
            .is_some_and(|badge| *badge != SkillBadge::DEVELOPER)
    }));
    assert!(friends.iter().any(|friend| friend.record.is_some()));
    assert!(friends.iter().any(|friend| friend.record.is_none()));
    assert!(friends.iter().any(|friend| {
        friend
            .main
            .as_ref()
            .is_some_and(|main| main.points.is_some())
    }));
    let rooms: Vec<&FriendRoom> = friends
        .iter()
        .filter_map(|friend| match &friend.presence {
            FriendPresence::Room { room, .. } => room.as_ref(),
            _ => None,
        })
        .collect();
    assert!(rooms.iter().any(|room| room.joinable));
    assert!(
        friends
            .iter()
            .any(|friend| matches!(friend.presence, FriendPresence::Offline { .. }))
    );
    assert_eq!(state.incoming.len(), 5);
    assert_eq!(
        state
            .incoming
            .iter()
            .filter(|request| request.played.is_some())
            .count(),
        3
    );
    assert_eq!(state.blocked.len(), 1);
}

#[tokio::test]
async fn the_statements_of_a_state_do_not_grow_with_the_friends() {
    let (_, _, few) = both(5).await;
    let (_, _, many) = both(150).await;
    assert_eq!(few, many);
    assert!(many <= 16, "{many} statements");
}
