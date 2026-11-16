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
    friends::parse_friend_code,
    levels::{CardSave, CardSearch, MAX_SEARCH, SearchAnswer, SearchHit},
};
use rusqlite::Transaction;

use crate::{
    AppState,
    cards::{self, Purpose, Reader, Viewer},
    error::Result,
    friends::{self, Classes, store},
    http::{Body, GENERAL_BODY, ok},
    routes::{
        account::{Bucket, Gate, Linked},
        rooms,
    },
    secrets::Keys,
};

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

/// `PUT /v1/cards/me`: replaces what the caller's card shows, its friend code
/// too where cards show codes (`cards::codes_shown`). 204.
pub async fn save(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let caller = Linked::admit(&state, &headers, Gate::Profiles, &SAVES).await?;
    let card: CardSave = body.parse()?;
    card.check()?;
    rooms::check_main(&rooms::game(&state), card.main)?;
    cards::check_friend_code(&state, &card)?;
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
    let caller = Linked::admit(&state, &headers, Gate::Profiles, &SEARCHES).await?;
    let search: CardSearch = body.parse()?;
    search.check()?;
    let keys = state.keys.clone();
    let friends_offered = friends::offered(&state);
    let codes_shown = cards::codes_shown(&state);
    let classes = Classes::of(&state, Some(&caller.account));
    let answer = state
        .db
        .read(move |tx| {
            let viewer = Viewer {
                caller: caller.account.as_str(),
                friends_offered,
                codes_shown,
                classes: &classes,
                now: caller.now,
            };
            find(tx, &keys, &viewer, &search)
        })
        .await?;
    Ok(ok(&answer))
}

/// The players `search` finds for the viewer, who has a Discord account: the
/// same few reads however many it finds (`cards::find_by_name`,
/// `cards::Reader`). A friend code names an account only while friends are
/// offered, as everywhere else.
pub(crate) fn find(
    tx: &Transaction<'_>,
    keys: &Keys,
    viewer: &Viewer<'_>,
    search: &CardSearch,
) -> Result<SearchAnswer> {
    let found = match (search.trimmed_name(), search.code.as_deref()) {
        (Some(name), _) => cards::find_by_name(tx, name, SCAN)?,
        (None, Some(code)) if viewer.friends_offered => {
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
    let accounts: Vec<&str> = found.iter().map(|hit| hit.account.as_str()).collect();
    let read = Reader::read(tx, viewer, &accounts, Purpose::Hits)?;
    let mut players = Vec::new();
    let mut more = false;
    for hit in &found {
        if hit.account == viewer.caller || read.relation(&hit.account).is_none() {
            continue;
        }
        let Some(card) = cards::issue(keys, &hit.account, viewer.now) else {
            continue;
        };
        if players.len() == MAX_SEARCH {
            more = true;
            break;
        }
        players.push(SearchHit {
            card,
            badge: read.badge(&hit.account),
            name: hit.name.clone(),
            country: hit.country.clone(),
            main: hit.main,
            since: cards::month_start(hit.since),
        });
    }
    Ok(SearchAnswer { players, more })
}
