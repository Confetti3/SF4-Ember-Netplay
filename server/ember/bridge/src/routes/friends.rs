//! The friends routes (docs/design/FRIENDS.md, "Wire contracts"): a sync of
//! the caller's presence and one action on their friends, each answered with
//! the whole `FriendsState`, as the desk's read and act are. Offered where
//! `friends::offered` says; elsewhere they answer 404, as a bridge from
//! before them does.
//!
//! tests/friends.rs scans every answer of these routes, and of every route
//! that serves one player's data to another, for the Discord IDs it seeded.
//! A new route of that kind joins its list.
use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::friends::{FriendAction, FriendsSync, PresenceReport};

use crate::{
    AppState,
    error::Result,
    friends::{self, Classes, store},
    http::{Body, GENERAL_BODY, ok},
    routes::account::{Bucket, Gate, Linked},
};

const SYNCS: Bucket = Bucket {
    name: "friends-sync",
    uses: 12,
    window: 60,
};
const ACTIONS: Bucket = Bucket {
    name: "friends-act",
    uses: 30,
    window: 10 * 60,
};
/// Requests by friend code, against guessing codes.
const CODE_REQUESTS: Bucket = Bucket {
    name: "friends-code",
    uses: 10,
    window: 60 * 60,
};
const INVITES: Bucket = Bucket {
    name: "friends-invite",
    uses: 10,
    window: 10 * 60,
};

/// `POST /v1/friends/sync`: the caller's presence in, the state out.
pub async fn sync(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let caller = Linked::admit(&state, &headers, Gate::Friends, &SYNCS).await?;
    let sync: FriendsSync = body.parse()?;
    sync.check()?;
    let FriendsSync {
        build_id,
        listing,
        main,
        presence,
    } = sync;
    let leaving = matches!(presence, PresenceReport::Offline {});
    let (account, now) = (caller.account.clone(), caller.now);
    state
        .db
        .write(move |tx| store::sync(tx, &account, &listing, main, leaving, now))
        .await?;
    state
        .friends
        .report(&caller.account, presence, build_id, now);
    answer(&state, caller).await
}

/// `POST /v1/friends/actions`: one action in, the state after it out.
pub async fn act(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let caller = Linked::admit(&state, &headers, Gate::Friends, &ACTIONS).await?;
    let action: FriendAction = body.parse()?;
    action.check()?;
    let own_bucket = match &action {
        FriendAction::Request { code: Some(_), .. } => Some(&CODE_REQUESTS),
        FriendAction::Invite { .. } => Some(&INVITES),
        _ => None,
    };
    if let Some(bucket) = own_bucket {
        caller.spend(&state, bucket)?;
    }
    let (live, keys) = (state.friends.clone(), state.keys.clone());
    let (account, now) = (caller.account.clone(), caller.now);
    let effect = state
        .db
        .write(move |tx| friends::act(tx, &live, &keys, &account, action, now))
        .await?;
    // What the action changes in memory waits for its commit.
    if let Some(effect) = effect {
        state.friends.apply(effect, now);
    }
    answer(&state, caller).await
}

/// The state `caller` sees after their sync or action.
async fn answer(state: &AppState, caller: Linked) -> Result<Response> {
    let live = state.friends.clone();
    let classes = Classes::of(state);
    let answer = state
        .db
        .read(move |tx| friends::state(tx, &live, &caller.account, &classes, caller.now))
        .await?;
    Ok(ok(&answer))
}
