//! The fighter level routes (docs/design/FIGHTER_LEVELS.md, "Wire
//! contracts"): a player's own levels and the look-up of other players'
//! cards, which friends extend with a friend's card and the caller's relation
//! to each player (docs/design/FRIENDS.md, "The card look-up"), and the
//! profile extends with a received request and a friend code as selectors and
//! with what a card says of its player (docs/design/PROFILE.md). Only where
//! public rooms are on, since only their games count; a bridge without them
//! answers 404, as one from before them does.
use axum::{extract::State, http::HeaderMap, response::Response};
use std::collections::HashMap;

use ember_protocol::{
    friends::parse_friend_code,
    levels::{CardAnswers, CardLookup, CardSelector, OwnLevels, Relation, RivalCard},
};
use rusqlite::Transaction;

use crate::{
    AppState,
    cards::{self, Purpose, Reader, Viewer},
    error::Result,
    friends::{self, Classes, store},
    http::{Body, GENERAL_BODY, ok},
    routes::account::{Bucket, Gate, PlayerAccount},
    secrets::Keys,
    skill::levels,
};

/// A player's own points. The game reads them at start, when the Profile
/// opens, and twice after each set it fought (8 and 20 seconds after its
/// end, `LevelRequests.hxx`), so a set of even a minute costs two: 30 in ten
/// minutes leaves room for opening the Profile as often as anyone does. Kept
/// apart from the look-ups, so a busy room's cards never cost a player their
/// own points.
const OWN: Bucket = Bucket {
    name: "points-me",
    uses: 30,
    window: 10 * 60,
};
/// Card look-ups. The game sends at most one every 10 seconds for a room's
/// members (`CardRequests.hxx`, `MinGapMs`), and one of friends alone every
/// 3 seconds while the Friends tab moves its focus: 120 in ten minutes is
/// twice a room's steady most, and room for opening profiles and looking
/// through friends besides. Each look-up costs the same few reads whatever
/// its number of cards (`cards::Reader`).
const LOOKUPS: Bucket = Bucket {
    name: "cards-lookup",
    uses: 120,
    window: 10 * 60,
};

/// `GET /v1/points/me`: the points as they are earned, however the build
/// asks (with the class bonus query or without). Read only: nothing is
/// remembered as shown.
pub async fn own(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let caller = PlayerAccount::admit(&state, &headers, Gate::Points, &OWN).await?;
    let now = caller.now;
    let keys = state.keys.clone();
    let friends_offered = friends::offered(&state);
    let classes = Classes::of(&state);
    let levels = state
        .db
        .read(move |tx| {
            let mut own = levels::own(tx, &caller.ember_id)?;
            if let OwnLevels::Counted { rival, .. } = &mut own
                && let Some(caller) = &caller.account
            {
                let viewer = Viewer {
                    caller: caller.as_str(),
                    friends_offered,
                    classes: &classes,
                    now,
                };
                *rival = most_played(tx, &keys, &viewer, caller)?;
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
    let read = Reader::read(tx, viewer, &others, Purpose::Rivals)?;
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
/// blocked the caller shows them no card, and once friends are offered each
/// card carries the caller's relation to its player. A block is between
/// Discord accounts, so a caller without one is shown no card at all
/// (docs/design/PROFILE.md, "Privacy").
pub async fn lookup(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let caller = PlayerAccount::admit(&state, &headers, Gate::Points, &LOOKUPS).await?;
    let lookup: CardLookup = body.parse()?;
    lookup.check()?;
    let (Some(account), now) = (caller.account, caller.now) else {
        return Ok(ok(&CardAnswers {
            cards: vec![None; lookup.cards.len()],
        }));
    };
    let keys = state.keys.clone();
    let friends_offered = friends::offered(&state);
    let classes = Classes::of(&state);
    let answers = state
        .db
        .read(move |tx| {
            let viewer = Viewer {
                caller: &account,
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
/// `cards::Reader`).
pub(crate) fn answer(
    tx: &Transaction<'_>,
    keys: &Keys,
    viewer: &Viewer<'_>,
    selectors: &[CardSelector],
) -> Result<CardAnswers> {
    let targets = targets_of(tx, keys, viewer, selectors)?;
    let accounts: Vec<&str> = targets.iter().flatten().map(String::as_str).collect();
    let read = Reader::read(tx, viewer, &accounts, Purpose::Card)?;
    Ok(CardAnswers {
        cards: targets
            .iter()
            .map(|target| target.as_deref().and_then(|target| read.card(target)))
            .collect(),
    })
}

/// The account each of `selectors` names for the viewer, if it names one they
/// may open, in order. A friend, a request and a code are the caller's own,
/// so they name someone only while friends are offered; a code never names
/// the caller. Each kind is one read for all its selectors, and none
/// when there are none of it.
fn targets_of(
    tx: &Transaction<'_>,
    keys: &Keys,
    viewer: &Viewer<'_>,
    selectors: &[CardSelector],
) -> Result<Vec<Option<String>>> {
    let own = viewer.friends_offered.then_some(viewer.caller);
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
