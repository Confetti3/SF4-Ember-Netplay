//! The profile routes (docs/design/PROFILE.md, "Wire contracts"): the caller
//! saves what their card shows, and searches for players by name or friend
//! code. Both need a Discord account, since a card belongs to one, and both
//! exist only where public rooms are on, beside `points`; a bridge without
//! them answers 404, as one from before them does.
//!
//! A search lists no one the caller may not see a card of: not the caller,
//! and not a player who blocked them. It never names an account: each player
//! comes with a fresh card token (`cards::issue`) that opens only here.
use axum::{
    extract::State,
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Response},
};
use ember_protocol::{
    api::ErrorCode,
    friends::parse_friend_code,
    levels::{CardSave, CardSearch, MAX_SEARCH, SearchAnswer, SearchHit},
    skill::NO_ACCOUNT,
};
use rusqlite::Transaction;

use crate::{
    AppState, cards,
    error::{ApiFailure, Result},
    friends::{self, Classes, store},
    http::{Body, GENERAL_BODY, ok},
    routes::{
        play::playing,
        rooms,
    },
    secrets::Keys,
    skill,
};

/// A rate limit: uses per window of seconds, by Discord account.
struct Bucket {
    name: &'static str,
    uses: usize,
    window: u64,
}

const SAVES: Bucket = Bucket {
    name: "cards-me",
    uses: 10,
    window: 10 * 60,
};
const SEARCHES: Bucket = Bucket {
    name: "cards-search",
    uses: 20,
    window: 60,
};

/// Accounts a name search reads before it stops: the most that can be listed
/// after the callers' own and blockers' are left out, with one more to say
/// that more matched.
const SCAN: usize = 200;

/// The calling player's Discord account and the time.
struct Caller {
    account: String,
    now: u64,
}

/// `PUT /v1/cards/me`: replaces what the caller's card shows. 204.
pub async fn save(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let caller = admit(&state, &headers, &SAVES).await?;
    let card: CardSave = body.parse()?;
    card.check()?;
    state
        .db
        .write(move |tx| cards::save(tx, &caller.account, &card, caller.now))
        .await?;
    Ok(StatusCode::NO_CONTENT.into_response())
}

/// `POST /v1/cards/search`: up to `MAX_SEARCH` players whose name is the
/// text or begins with it, or the one whose friend code it is.
pub async fn search(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let caller = admit(&state, &headers, &SEARCHES).await?;
    let search: CardSearch = body.parse()?;
    search.check()?;
    let keys = state.keys.clone();
    let friends_offered = friends::offered(&state);
    let classes = Classes::of(&state);
    let answer = state
        .db
        .read(move |tx| find(tx, &keys, &classes, friends_offered, &caller, &search))
        .await?;
    Ok(ok(&answer))
}

/// The players `search` finds for `caller`. A friend code names an account
/// only while friends are offered, as everywhere else.
fn find(
    tx: &Transaction<'_>,
    keys: &Keys,
    classes: &Classes,
    friends_offered: bool,
    caller: &Caller,
    search: &CardSearch,
) -> Result<SearchAnswer> {
    let found = match (search.trimmed_name(), search.code.as_deref()) {
        (Some(name), _) => cards::find_by_name(tx, name, SCAN)?,
        (None, Some(code)) if friends_offered => {
            match parse_friend_code(code)
                .map(|code| store::account_of_code(tx, &code))
                .transpose()?
                .flatten()
            {
                Some(account) => cards::found(tx, &account)?.into_iter().collect(),
                None => Vec::new(),
            }
        }
        _ => Vec::new(),
    };
    let mut players = Vec::new();
    let mut more = false;
    for hit in found {
        if hit.account == caller.account
            || store::relation(tx, &caller.account, &hit.account)?.is_none()
        {
            continue;
        }
        let Some(card) = cards::issue(keys, &hit.account, caller.now) else {
            continue;
        };
        if players.len() == MAX_SEARCH {
            more = true;
            break;
        }
        players.push(SearchHit {
            card,
            badge: cards::badge(tx, &hit.account, classes, caller.now)?,
            name: hit.name,
            country: hit.country,
            main: hit.main,
            since: cards::month_start(hit.since),
        });
    }
    Ok(SearchAnswer { players, more })
}

/// The calling player's Discord account, within `bucket`.
async fn admit(state: &AppState, headers: &HeaderMap, bucket: &Bucket) -> Result<Caller> {
    if !rooms::enabled(state) {
        return Err(ApiFailure::not_found());
    }
    let ember_id = playing(state, headers).await?.ember_id;
    let account = state
        .db
        .read(move |tx| skill::account(tx, &ember_id))
        .await?
        .ok_or_else(|| {
            ApiFailure::new(
                ErrorCode::Forbidden,
                "Connect a Discord account to have a card.",
            )
            .detail("reason", NO_ACCOUNT)
        })?;
    let now = state.now();
    state
        .rate
        .check(
            &format!("{}:{account}", bucket.name),
            bucket.uses,
            bucket.window,
            now,
        )
        .map_err(ApiFailure::rate_limited)?;
    Ok(Caller { account, now })
}
