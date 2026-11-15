//! Friends (docs/design/FRIENDS.md): players who added each other see where
//! the other is, join or watch them, and invite them into their own room.
//! Friends belong to the Discord account connected to the Ember ID, like the
//! rating, so a new Ember ID keeps them and a throwaway one has none.
//!
//! The tables are `store`'s, what is kept in memory only is `presence`'s;
//! this module carries out a player's action on the tables and puts together
//! the `FriendsState` every friends answer is. Nothing it answers names an
//! account: friendships, requests, blocks and invites are named by random
//! ids, and players by the name and flag their own game sends.
pub mod presence;
pub mod store;

use std::collections::HashMap;

use ember_protocol::{
    Error,
    friends::{
        DEFAULT_GAME, Friend, FriendAction, FriendPresence, FriendRefusal, FriendRoom,
        FriendsState, IncomingRequest, Joining, MainFighter, Played, PrivateRoom, ReceivedInvite,
        parse_friend_code,
    },
    levels::{CardAccount, PlayerCard},
};
use rusqlite::Transaction;

use self::{
    presence::{Doing, Effect, Live, Seen},
    store::{FriendRow, refuse},
};
use crate::{
    AppState, Keys, cards,
    error::{ApiFailure, Result},
    linking::Site,
    routes::{rooms, sign_in},
};

/// Whether friends are offered: where public rooms are on and `rooms.friends`
/// is set, beside `points`, and Discord sign-in is offered, since friends
/// belong to a Discord account. The one place the switch is read: discovery,
/// the friends routes and the card look-up all ask this.
pub fn offered(state: &AppState) -> bool {
    rooms::enabled(state)
        && state
            .config
            .rooms
            .as_ref()
            .is_some_and(|rooms| rooms.friends)
        && sign_in::offered(state, &Site::Discord)
}

/// What a state shows of others' classes: the bridge's `rooms.skill_badges`
/// and its developers.
pub struct Classes {
    pub shown: bool,
    pub developers: Vec<String>,
}

impl Classes {
    /// The bridge's own settings, which every route that shows a class reads
    /// the same way.
    pub fn of(state: &AppState) -> Self {
        Self {
            shown: rooms::classes_shown(state),
            developers: rooms::developers(state),
        }
    }
}

/// An action before the caller ever synced: there is no state to answer yet.
fn resync() -> ApiFailure {
    ApiFailure::new(
        ember_protocol::api::ErrorCode::ResyncRequired,
        "Sync friends before acting on them.",
    )
}

/// Carries out `action` for Discord account `caller` in `tx`. An action naming
/// a request, friendship, block or invite that is not the caller's changes
/// nothing: the state answered shows how things stand. What it changes in
/// memory comes back as an `Effect`, for the route to make once `tx` has
/// committed.
///
/// An account that turned friends off refuses everything but `TurnOn` as
/// `friends_off`. `TurnOn` needs the account's row, which only a sync makes:
/// the game that saw `friends_off` has synced before, so this never asks it
/// for a listing the action does not carry.
pub fn act(
    tx: &Transaction<'_>,
    live: &Live,
    keys: &Keys,
    caller: &str,
    action: FriendAction,
    now: u64,
) -> Result<Option<Effect>> {
    let own = store::account(tx, caller)?.ok_or_else(resync)?;
    if own.off && !matches!(action, FriendAction::TurnOn {}) {
        return Err(refuse(FriendRefusal::FriendsOff));
    }
    let stored = |done: Result<()>| done.map(|()| None);
    match action {
        FriendAction::Request { card, label, code } => {
            let (target, label) = match (card, label, code) {
                (Some(card), Some(label), None) => (opened(keys, &card, now)?, label),
                (None, None, Some(typed)) => {
                    let code = parse_friend_code(&typed).ok_or(Error::InvalidField("code"))?;
                    let target = store::account_of_code(tx, &code)?
                        .ok_or_else(|| refuse(FriendRefusal::UnknownCode))?;
                    (target, code)
                }
                _ => return Err(Error::InvalidField("request").into()),
            };
            stored(store::ask(tx, caller, &target, &label, now))
        }
        FriendAction::Accept { request } => stored(store::accept(tx, caller, &request, now)),
        FriendAction::Decline { request, block } => {
            stored(store::decline(tx, caller, &request, block, now))
        }
        FriendAction::Cancel { request } => stored(store::cancel(tx, caller, &request, now)),
        FriendAction::Remove { friend, block } => Ok(store::remove(
            tx, caller, &friend, block, now,
        )?
        .map(|other| Effect::Unpair {
            one: caller.to_owned(),
            other,
        })),
        FriendAction::Block { card, label } => {
            let target = opened(keys, &card, now)?;
            store::block(tx, caller, &target, &store::listed_name(&label), now)?;
            Ok(Some(Effect::Unpair {
                one: caller.to_owned(),
                other: target,
            }))
        }
        FriendAction::Unblock { block } => stored(store::unblock(tx, caller, &block)),
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
            let invitee = reachable.ok_or_else(|| refuse(FriendRefusal::InviteUnavailable))?;
            Ok(Some(Effect::Invite {
                inviter: caller.to_owned(),
                invitee,
                room,
            }))
        }
        // Hands out a secret, so it has its own answer (`join`), which the
        // route calls; here it is not an action that changes anything.
        FriendAction::Join { .. } => Err(Error::InvalidField("action").into()),
        FriendAction::Dismiss { invite } => Ok(Some(Effect::Dismiss {
            invitee: caller.to_owned(),
            id: invite,
        })),
        FriendAction::AppearOffline { on } => stored(store::appear_offline(tx, caller, on, now)),
        FriendAction::NewCode {} => stored(store::new_code(tx, caller, now)),
        FriendAction::Forget {} => {
            store::forget(tx, caller, now)?;
            Ok(Some(Effect::Forget {
                account: caller.to_owned(),
            }))
        }
        FriendAction::TurnOn {} => stored(store::turn_on(tx, caller, now)),
    }
}

/// The invitation to the private room of the friend `friend_id` (`fr_`), for
/// `caller` who pressed join. It is what the bridge hands out of the offer a
/// friend's game reported, and only here: no state carries it otherwise.
///
/// The check is the invite's reachability (still friends, not appearing
/// offline, synced lately) and the visit's: the same game and build as the
/// caller's last sync, a private room on offer, and a seat free. Every way it
/// fails is `join_unavailable`, so the caller learns nothing about which; an
/// account with friends off is refused as `friends_off`, as every route does.
pub fn join(
    tx: &Transaction<'_>,
    live: &Live,
    caller: &str,
    friend_id: &str,
    now: u64,
) -> Result<Joining> {
    let own = store::account(tx, caller)?.ok_or_else(resync)?;
    if own.off {
        return Err(refuse(FriendRefusal::FriendsOff));
    }
    let viewer = live.seen(caller).ok_or_else(resync)?;
    let unavailable = || refuse(FriendRefusal::JoinUnavailable);
    let other = store::friend_of(tx, caller, friend_id)?.ok_or_else(unavailable)?;
    let shown = store::account(tx, &other)?.ok_or_else(unavailable)?;
    if shown.appear_offline {
        return Err(unavailable());
    }
    let seen = live
        .seen(&other)
        .filter(|seen| seen.online(now))
        .ok_or_else(unavailable)?;
    if seen.game != viewer.game || seen.build_id != viewer.build_id {
        return Err(unavailable());
    }
    let Doing::Room(room) = seen.doing else {
        return Err(unavailable());
    };
    let offer = room
        .offer
        .filter(|_| room.private)
        .filter(|offer| offer.members < offer.capacity)
        .ok_or_else(unavailable)?;
    Ok(Joining {
        friend: friend_id.to_owned(),
        secret: offer.secret,
    })
}

/// The account a card token names, or `card_expired`.
fn opened(keys: &Keys, card: &str, now: u64) -> Result<String> {
    cards::open(keys, card, now).ok_or_else(|| refuse(FriendRefusal::CardExpired))
}

/// The game and build of the viewer's last sync: what decides whose rooms a
/// state shows and whether they are joinable.
struct Viewing<'a> {
    game: &'a str,
    build: Option<&'a str>,
}

/// Everything `caller` sees of their friends now. The caller's game and
/// build, for whose rooms are shown and whether a friend's room is joinable,
/// are the ones their last sync named. A caller the bridge has not heard from
/// since it started is read as the default game's with no build, so an older
/// build's first answer shows what it always did.
///
/// It costs the same few reads whatever the number of friends: the caller's
/// own lists, the friends' points, classes and records, and the senders'
/// records (`cards::Reader`), and the friends' rooms (`store::listed_rooms`),
/// all in the one transaction `tx`.
pub fn state(
    tx: &Transaction<'_>,
    live: &Live,
    caller: &str,
    classes: &Classes,
    now: u64,
) -> Result<FriendsState> {
    let own = store::account(tx, caller)?.ok_or_else(resync)?;
    // Also what answers the `Forget` that just turned friends off.
    if own.off {
        return Err(refuse(FriendRefusal::FriendsOff));
    }
    let last = live.seen(caller);
    let viewing = Viewing {
        game: last
            .as_ref()
            .map_or(DEFAULT_GAME, |seen| seen.game.as_str()),
        build: last.as_ref().map(|seen| seen.build_id.as_str()),
    };
    let rows = store::friends(tx, caller)?;
    let requests = store::incoming(tx, caller)?;
    let accounts: Vec<&str> = rows
        .iter()
        .map(|row| row.account.as_str())
        .chain(requests.iter().map(|row| row.sender.as_str()))
        .collect();
    let viewer = cards::Viewer {
        caller,
        friends_offered: true,
        classes,
        now,
    };
    let read = cards::Reader::read(tx, &viewer, &accounts, cards::Purpose::Friends)?;
    let seen: Vec<Option<Seen>> = rows.iter().map(|row| visible(live, row, now)).collect();
    let room_ids: Vec<&str> = seen
        .iter()
        .flatten()
        .filter_map(|seen| match &seen.doing {
            Doing::Room(room) => room.room_id.as_deref(),
            Doing::Activity(_) => None,
        })
        .collect();
    let rooms = store::listed_rooms(tx, &room_ids, viewing.build, viewing.game)?;
    let mut friends = Vec::with_capacity(rows.len());
    let mut ids = HashMap::with_capacity(rows.len());
    for (row, seen) in rows.into_iter().zip(seen) {
        let (main, account) = levels_of(&row, read.points(&row.account));
        friends.push(Friend {
            id: row.id.clone(),
            since: row.since,
            main,
            account,
            badge: read.badge(&row.account),
            presence: presence_of(&row, seen, &viewing, &rooms),
            record: read.record(&row.account),
            name: row.shown.name,
            country: row.shown.country,
        });
        ids.insert(row.account, row.id);
    }
    let incoming = requests
        .into_iter()
        .map(|row| IncomingRequest {
            played: read.record(&row.sender).map(|record| Played {
                games: record.won + record.lost,
                last_at: record.last_at,
            }),
            id: row.id,
            name: row.name,
            country: row.country,
            sent_at: row.sent_at,
        })
        .collect();
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
        joining: None,
    };
    state.check().map_err(|_| ApiFailure::unavailable())?;
    Ok(state)
}

/// A friend's main fighter, with its points and tier once it has been played,
/// and their account's points and tier: both from their card, which is public.
/// A friend with no card (their Discord account was disconnected) has no
/// account points, and their main shows none.
fn levels_of(
    friend: &FriendRow,
    card: Option<&PlayerCard>,
) -> (Option<MainFighter>, Option<CardAccount>) {
    let main = friend.shown.main.map(|fighter| {
        let played = card.as_ref().and_then(|card| {
            card.fighters
                .iter()
                .find(|played| played.fighter == fighter)
        });
        MainFighter {
            fighter,
            points: played.map(|played| played.points),
            tier: played.map(|played| played.tier),
        }
    });
    (main, card.map(|card| card.account))
}

/// What a friend's last sync said, while the caller may see it: not while the
/// friend appears offline, or has not synced lately.
fn visible(live: &Live, friend: &FriendRow, now: u64) -> Option<Seen> {
    live.seen(&friend.account)
        .filter(|seen| !friend.shown.appear_offline && seen.online(now))
}

/// Where a friend is, as the caller may see it: offline, with when they were
/// last seen, while they appear offline or have not synced lately (`seen` is
/// none); else what their last sync said and the game it was for. A friend in
/// the viewer's game has their public room while it is open and listed
/// (`rooms`) and their private room's facts while they offer it, joinable
/// when it is of the viewer's build and has a seat free; a friend in another
/// game shows only what they are doing, and no room.
fn presence_of(
    friend: &FriendRow,
    seen: Option<Seen>,
    viewing: &Viewing<'_>,
    rooms: &HashMap<String, FriendRoom>,
) -> FriendPresence {
    let Some(seen) = seen else {
        return FriendPresence::Offline {
            seen_at: Some(friend.shown.seen_at),
        };
    };
    let same_game = seen.game == viewing.game;
    let same_build = viewing.build == Some(seen.build_id.as_str());
    match seen.doing {
        Doing::Activity(activity) => FriendPresence::Online {
            activity,
            game: Some(seen.game),
        },
        Doing::Room(room) if !same_game => FriendPresence::Room {
            status: room.status,
            table: room.table,
            room: None,
            private: room.private,
            game: Some(seen.game),
            private_room: None,
        },
        Doing::Room(room) => {
            let private_room =
                room.offer
                    .as_ref()
                    .filter(|_| room.private)
                    .map(|offer| PrivateRoom {
                        name: offer.name.clone(),
                        members: offer.members,
                        capacity: offer.capacity,
                        joinable: same_build && offer.members < offer.capacity,
                    });
            FriendPresence::Room {
                status: room.status,
                table: room.table,
                room: room
                    .room_id
                    .and_then(|room_id| rooms.get(&room_id).cloned()),
                private: room.private,
                game: Some(seen.game),
                private_room,
            }
        }
    }
}

#[cfg(test)]
mod state_tests;
