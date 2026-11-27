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
    routes::account::{Bucket, Gate, Linked},
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
pub(crate) const SCAN: usize = 200;

/// `PUT /v1/cards/me`: replaces what the caller's card shows. 204.
pub async fn save(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let caller = Linked::admit(&state, &headers, Gate::Profiles, &SAVES).await?;
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
    let caller = Linked::admit(&state, &headers, Gate::Profiles, &SEARCHES).await?;
    let search: CardSearch = body.parse()?;
    search.check()?;
    let keys = state.keys.clone();
    let friends_offered = friends::offered(&state);
    let classes = Classes::of(&state);
    let answer = state
        .db
        .read(move |tx| {
            let viewer = Viewer {
                caller: caller.account.as_str(),
                friends_offered,
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
    let (shown, read) = shown_to(tx, viewer, found)?;
    let mut hits = shown
        .iter()
        .filter_map(|found| hit(keys, &read, found, viewer.now));
    let players: Vec<SearchHit> = hits.by_ref().take(MAX_SEARCH).collect();
    let more = hits.next().is_some();
    Ok(SearchAnswer { players, more })
}

/// Of `found`, the players the viewer may be shown, in order, and what was
/// read of them (`Purpose::Hits`): not the viewer, and not a player who
/// blocked them.
pub(crate) fn shown_to(
    tx: &Transaction<'_>,
    viewer: &Viewer<'_>,
    found: Vec<cards::Found>,
) -> Result<(Vec<cards::Found>, Reader)> {
    let read = {
        let accounts: Vec<&str> = found.iter().map(|hit| hit.account.as_str()).collect();
        Reader::read(tx, viewer, &accounts, Purpose::Hits)?
    };
    let shown = found
        .into_iter()
        .filter(|hit| hit.account != viewer.caller && read.relation(&hit.account).is_some())
        .collect();
    Ok((shown, read))
}

/// `found` as a search lists them, with a fresh card token. None for an
/// account no token names.
pub(crate) fn hit(keys: &Keys, read: &Reader, found: &cards::Found, now: u64) -> Option<SearchHit> {
    Some(SearchHit {
        card: cards::issue(keys, &found.account, now)?,
        badge: read.badge(&found.account),
        name: found.name.clone(),
        country: found.country.clone(),
        main: found.main,
        since: cards::month_start(found.since),
    })
}
