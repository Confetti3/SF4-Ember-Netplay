//! The fighter level routes (docs/design/FIGHTER_LEVELS.md, "Wire
//! contracts"): a player's own levels and the look-up of other players'
//! cards, which friends extend with a friend's card and the caller's relation
//! to each player (docs/design/FRIENDS.md, "The card look-up"), and the
//! profile extends with a received request and a friend code as selectors and
//! with what a card says of its player (docs/design/PROFILE.md). Only where
//! public rooms are on, since only their games count; a bridge without them
//! answers 404, as one from before them does.
use axum::{
    extract::{RawQuery, State},
    http::HeaderMap,
    response::Response,
};
use std::collections::HashMap;

use ember_protocol::{
    friends::parse_friend_code,
    levels::{
        CLASS_BONUS_QUERY, CardAnswers, CardLookup, CardSelector, OwnLevels, Relation, RivalCard,
    },
};
use rusqlite::Transaction;

use crate::{
    AppState,
    auth::Player,
    cards::{self, Parts, Reader, Viewer},
    error::{ApiFailure, Result},
    friends::{self, Classes, store},
    http::{Body, GENERAL_BODY, ok},
    routes::{play::playing, rooms},
    secrets::Keys,
    skill::{self, levels},
};

/// Level requests one player may make per window, both routes together.
const REQUESTS_PER_WINDOW: usize = 60;
const WINDOW_SECS: u64 = 10 * 60;

/// `GET /v1/points/me`. Read only: nothing is remembered as shown. A build
/// that reads class bonus points says so in the query and is answered the
/// points as they are earned; any other is one from before the bonus, which
/// refuses points that are not games plus wins, and is answered that sum.
pub async fn own(
    State(state): State<AppState>,
    RawQuery(query): RawQuery,
    headers: HeaderMap,
) -> Result<Response> {
    let (player, now) = admit(&state, &headers).await?;
    let keys = state.keys.clone();
    let friends_offered = friends::offered(&state);
    let classes = Classes::of(&state);
    let sum = if query.as_deref() == Some(CLASS_BONUS_QUERY) {
        levels::Sum::Earned
    } else {
        levels::Sum::GamesAndWins
    };
    let levels = state
        .db
        .read(move |tx| {
            let mut own = levels::own(tx, &player.ember_id, sum)?;
            if let OwnLevels::Counted { rival, .. } = &mut own
                && let Some(caller) = skill::account(tx, &player.ember_id)?
            {
                let viewer = Viewer {
                    caller: Some(caller.as_str()),
                    friends_offered,
                    classes: &classes,
                    now,
                };
                *rival = most_played(tx, &keys, &viewer, &caller)?;
            }
            Ok(own)
        })
        .await?;
    Ok(ok(&levels))
}

/// The opponent `caller` played the most, as the caller's own answer names
/// them: the first of their `levels::RIVAL_CANDIDATES` most played pairs that
/// neither side blocked, whose player has a card to show a name from, and
/// whose card is well formed. None when no pair can be shown. The opponent
/// comes as a fresh card token, never as an account.
fn most_played(
    tx: &Transaction<'_>,
    keys: &Keys,
    viewer: &Viewer<'_>,
    caller: &str,
) -> Result<Option<RivalCard>> {
    let pairs = levels::pairs_by_games(tx, caller, levels::RIVAL_CANDIDATES)?;
    let others: Vec<&str> = pairs.iter().map(|pair| pair.other.as_str()).collect();
    let read = Reader::read(tx, viewer, &others, Parts::RIVALS)?;
    for pair in &pairs {
        // A block either way passes the pair over: `relation` is none when
        // they blocked the caller and `Blocked` when the caller blocked them.
        if read
            .relation(&pair.other)
            .is_none_or(|relation| relation == Relation::Blocked)
        {
            continue;
        }
        let Some(found) = read.listing(&pair.other) else {
            continue;
        };
        let Some(card) = cards::issue(keys, &pair.other, viewer.now) else {
            continue;
        };
        let rival = RivalCard {
            card,
            name: found.name.clone(),
            country: found.country.clone(),
            badge: read.badge(&pair.other),
            won: pair.won,
            lost: pair.lost,
        };
        if rival.check().is_ok() {
            return Ok(Some(rival));
        }
    }
    Ok(None)
}

/// `POST /v1/cards/lookup`: one card, or null, per selector, in order. A
/// friend id names the caller's friend, a request id a request the caller
/// received, and a code a friend code, once friends are offered. A player who
/// blocked the caller shows them no card, a caller without a Discord account
/// is shown none (a block could not hold against them), and once friends are
/// offered each card carries the caller's relation to its player.
pub async fn lookup(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let (player, now) = admit(&state, &headers).await?;
    let lookup: CardLookup = body.parse()?;
    lookup.check()?;
    let keys = state.keys.clone();
    let friends_offered = friends::offered(&state);
    let classes = Classes::of(&state);
    let answers = state
        .db
        .read(move |tx| {
            let caller = skill::account(tx, &player.ember_id)?;
            let viewer = Viewer {
                caller: caller.as_deref(),
                friends_offered,
                classes: &classes,
                now,
            };
            answer(tx, &keys, &viewer, &lookup.cards)
        })
        .await?;
    Ok(ok(&answers))
}

/// The cards `selectors` name for `viewer`, one entry per selector, in
/// order: the same few reads however many there are (`targets_of`,
/// `cards::Reader`). A caller without a Discord account is shown none
/// (`Reader::card`), so nothing is read for them.
pub(crate) fn answer(
    tx: &Transaction<'_>,
    keys: &Keys,
    viewer: &Viewer<'_>,
    selectors: &[CardSelector],
) -> Result<CardAnswers> {
    if viewer.caller.is_none() {
        return Ok(CardAnswers {
            cards: vec![None; selectors.len()],
        });
    }
    let targets = targets_of(tx, keys, viewer, selectors)?;
    let accounts: Vec<&str> = targets.iter().flatten().map(String::as_str).collect();
    let read = Reader::read(tx, viewer, &accounts, Parts::CARD)?;
    Ok(CardAnswers {
        cards: targets
            .iter()
            .map(|target| target.as_deref().and_then(|target| read.card(target)))
            .collect(),
    })
}

/// The account each of `selectors` names for the viewer, if it names one they
/// may open, in order. A friend, a request and a code are the caller's own,
/// so they need their Discord account and friends being offered; a code never
/// names the caller. Each kind is one read for all its selectors, and none
/// when there are none of it.
fn targets_of(
    tx: &Transaction<'_>,
    keys: &Keys,
    viewer: &Viewer<'_>,
    selectors: &[CardSelector],
) -> Result<Vec<Option<String>>> {
    let own = viewer.caller.filter(|_| viewer.friends_offered);
    let codes: Vec<Option<String>> = selectors
        .iter()
        .map(|selector| match selector {
            CardSelector::Code(code) => parse_friend_code(code),
            _ => None,
        })
        .collect();
    let (mut friend_ids, mut request_ids) = (Vec::new(), Vec::new());
    for selector in selectors {
        match selector {
            CardSelector::Friend(id) => friend_ids.push(id.as_str()),
            CardSelector::Request(id) => request_ids.push(id.as_str()),
            CardSelector::Card(_) | CardSelector::Code(_) => {}
        }
    }
    let code_ids: Vec<&str> = codes.iter().flatten().map(String::as_str).collect();
    let (mut friends, mut requests, mut holders) = (HashMap::new(), HashMap::new(), HashMap::new());
    if let Some(caller) = own {
        if !friend_ids.is_empty() {
            friends = store::friends_by_id(tx, caller, &friend_ids)?;
        }
        if !request_ids.is_empty() {
            requests = store::received_from(tx, caller, &request_ids)?;
        }
        if !code_ids.is_empty() {
            holders = store::accounts_of_codes(tx, &code_ids)?;
        }
    }
    Ok(selectors
        .iter()
        .zip(&codes)
        .map(|(selector, code)| match selector {
            CardSelector::Card(token) => cards::open(keys, token, viewer.now),
            _ if own.is_none() => None,
            CardSelector::Friend(id) => friends.get(id).cloned(),
            CardSelector::Request(id) => requests.get(id).cloned(),
            CardSelector::Code(_) => code
                .as_ref()
                .and_then(|code| holders.get(code).cloned())
                .filter(|target| Some(target.as_str()) != own),
        })
        .collect())
}

/// The calling player, within their request budget, and the time.
async fn admit(state: &AppState, headers: &HeaderMap) -> Result<(Player, u64)> {
    if !rooms::enabled(state) {
        return Err(ApiFailure::not_found());
    }
    let player = playing(state, headers).await?;
    let now = state.now();
    state
        .rate
        .check(
            &format!("points:{}", player.ember_id.as_str()),
            REQUESTS_PER_WINDOW,
            WINDOW_SECS,
            now,
        )
        .map_err(ApiFailure::rate_limited)?;
    Ok((player, now))
}
