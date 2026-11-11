//! Friends (docs/design/FRIENDS.md): players who added each other see where
//! the other is, join or watch them, and invite them into their own room.
//! Friends belong to the Discord account connected to the Ember ID, like the
//! rating, so a new Ember ID keeps them and a throwaway one has none.
//!
//! The tables are `store`'s, what is kept in memory only is `presence`'s;
//! this module carries out a player's action on both and puts together the
//! `FriendsState` every friends answer is. Nothing it answers names an
//! account: friendships, requests, blocks and invites are named by random
//! ids, and players by the name and flag their own game sends.
pub mod presence;
pub mod store;

use std::collections::HashMap;

use ember_protocol::{
    Error,
    friends::{
        CARD_EXPIRED, FriendAction, FriendPresence, FriendsState, INVITE_UNAVAILABLE,
        IncomingRequest, MainFighter, Played, ReceivedInvite, UNKNOWN_CODE, parse_friend_code,
    },
    skill::SkillBadge,
};
use rusqlite::Transaction;

use self::{
    presence::{Doing, Live},
    store::{FriendRow, refuse},
};
use crate::{
    AppState, Keys, cards,
    error::{ApiFailure, Result},
    linking::Site,
    routes::{rooms, sign_in},
    skill,
};

/// Whether friends are offered: where public rooms are on, beside `levels`,
/// and Discord sign-in is offered, since friends belong to a Discord account.
pub fn offered(state: &AppState) -> bool {
    rooms::enabled(state) && sign_in::offered(state, &Site::Discord)
}

/// What a state shows of others' classes: the bridge's `rooms.skill_badges`
/// and its developers.
pub struct Classes {
    pub shown: bool,
    pub developers: Vec<String>,
}

/// An action before the caller ever synced: there is no state to answer yet.
fn resync() -> ApiFailure {
    ApiFailure::new(
        ember_protocol::api::ErrorCode::ResyncRequired,
        "Sync friends before acting on them.",
    )
}

/// Carries out `action` for Discord account `caller`, in `tx` and in memory.
/// An action naming a request, friendship, block or invite that is not the
/// caller's changes nothing: the state answered shows how things stand.
pub fn act(
    tx: &Transaction<'_>,
    live: &Live,
    keys: &Keys,
    caller: &str,
    action: FriendAction,
    now: u64,
) -> Result<()> {
    store::account(tx, caller)?.ok_or_else(resync)?;
    match action {
        FriendAction::Request { card, label, code } => {
            let (target, label) = match (card, label, code) {
                (Some(card), Some(label), None) => (opened(keys, &card, now)?, label),
                (None, None, Some(typed)) => {
                    let code = parse_friend_code(&typed).ok_or(Error::InvalidField("code"))?;
                    let target = store::account_of_code(tx, &code)?
                        .ok_or_else(|| refuse(UNKNOWN_CODE))?;
                    (target, code)
                }
                _ => return Err(Error::InvalidField("request").into()),
            };
            store::ask(tx, caller, &target, &label, now)
        }
        FriendAction::Accept { request } => store::accept(tx, caller, &request, now),
        FriendAction::Decline { request, block } => {
            store::decline(tx, caller, &request, block, now)
        }
        FriendAction::Cancel { request } => store::cancel(tx, caller, &request, now),
        FriendAction::Remove { friend, block } => {
            if let Some(other) = store::remove(tx, caller, &friend, block, now)? {
                live.unpair(caller, &other);
            }
            Ok(())
        }
        FriendAction::Block { card, label } => {
            let target = opened(keys, &card, now)?;
            store::block(tx, caller, &target, &store::listed_name(&label), now)?;
            live.unpair(caller, &target);
            Ok(())
        }
        FriendAction::Unblock { block } => store::unblock(tx, caller, &block),
        FriendAction::Invite { friend, room } => {
            let reachable = match store::friend_of(tx, caller, &friend)? {
                Some(other) => {
                    let shown = store::account(tx, &other)?;
                    let present = shown.is_some_and(|shown| !shown.appear_offline)
                        && live.online(&other, now);
                    present.then_some(other)
                }
                None => None,
            };
            let invitee = reachable.ok_or_else(|| refuse(INVITE_UNAVAILABLE))?;
            live.invite(caller, &invitee, room, now);
            Ok(())
        }
        FriendAction::Dismiss { invite } => {
            live.dismiss(caller, &invite);
            Ok(())
        }
        FriendAction::AppearOffline { on } => store::appear_offline(tx, caller, on, now),
        FriendAction::NewCode {} => store::new_code(tx, caller, now),
        FriendAction::Forget {} => {
            store::forget(tx, caller, now)?;
            live.forget(caller);
            Ok(())
        }
    }
}

/// The account a card token names, or `card_expired`.
fn opened(keys: &Keys, card: &str, now: u64) -> Result<String> {
    cards::open(keys, card, now).ok_or_else(|| refuse(CARD_EXPIRED))
}

/// Everything `caller` sees of their friends now. The caller's build, for
/// whether a friend's room is joinable, is the one their last sync named.
pub fn state(
    tx: &Transaction<'_>,
    live: &Live,
    caller: &str,
    classes: &Classes,
    now: u64,
) -> Result<FriendsState> {
    let own = store::account(tx, caller)?.ok_or_else(resync)?;
    let build = live.seen(caller).map(|seen| seen.build_id);
    let rows = store::friends(tx, caller)?;
    let mut friends = Vec::with_capacity(rows.len());
    let mut ids = HashMap::with_capacity(rows.len());
    for row in rows {
        friends.push(ember_protocol::friends::Friend {
            id: row.id.clone(),
            since: row.since,
            main: main_of(tx, &row)?,
            badge: badge_of(tx, &row.account, classes, now)?,
            presence: presence_of(tx, live, &row, build.as_deref(), now)?,
            record: store::record(tx, caller, &row.account)?,
            name: row.shown.name,
            country: row.shown.country,
        });
        ids.insert(row.account, row.id);
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
    // An invite stands only while its sender is a friend.
    let invites = live
        .invites_for(caller, now)
        .into_iter()
        .filter_map(|invite| {
            Some(ReceivedInvite {
                friend: ids.get(&invite.inviter)?.clone(),
                id: invite.id,
                room: invite.room,
                expires_at: invite.expires_at,
            })
        })
        .collect();
    let state = FriendsState {
        code: own.code,
        appear_offline: own.appear_offline,
        friends,
        incoming,
        outgoing: store::outgoing(tx, caller)?,
        invites,
        blocked: store::blocked(tx, caller)?,
    };
    state.check().map_err(|_| ApiFailure::unavailable())?;
    Ok(state)
}

/// A friend's main fighter, with its points and tier while they show points.
fn main_of(tx: &Transaction<'_>, friend: &FriendRow) -> Result<Option<MainFighter>> {
    let Some(fighter) = friend.shown.main else {
        return Ok(None);
    };
    let shown = skill::levels::card(tx, &friend.account)?.and_then(|card| card.fighters);
    let played = shown
        .into_iter()
        .flatten()
        .find(|played| played.fighter == fighter);
    Ok(Some(MainFighter {
        fighter,
        points: played.map(|played| played.points),
        tier: played.map(|played| played.tier),
    }))
}

fn badge_of(
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

/// Where a friend is, as the caller may see it: offline, with when they were
/// last seen, while they appear offline or have not synced lately; else what
/// their last sync said, with their public room while it is open and listed.
fn presence_of(
    tx: &Transaction<'_>,
    live: &Live,
    friend: &FriendRow,
    build: Option<&str>,
    now: u64,
) -> Result<FriendPresence> {
    let seen = live
        .seen(&friend.account)
        .filter(|seen| !friend.shown.appear_offline && seen.online(now));
    let Some(seen) = seen else {
        return Ok(FriendPresence::Offline {
            seen_at: Some(friend.shown.seen_at),
        });
    };
    Ok(match seen.doing {
        Doing::Activity(activity) => FriendPresence::Online { activity },
        Doing::Room(room) => FriendPresence::Room {
            status: room.status,
            table: room.table,
            room: match room.room_id {
                Some(room_id) => store::listed_room(tx, &room_id, build)?,
                None => None,
            },
            private: room.private,
        },
    })
}
