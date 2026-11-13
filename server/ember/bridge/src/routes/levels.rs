//! The fighter level routes (docs/design/FIGHTER_LEVELS.md, "Wire
//! contracts"): a player's own levels and the look-up of other players'
//! cards, which friends extend with a friend's card and the caller's relation
//! to each player (docs/design/FRIENDS.md, "The card look-up"), and the
//! profile extends with a received request and a friend code as selectors and
//! with what a card says of its player (docs/design/PROFILE.md). Only where
//! public rooms are on, since only their games count; a bridge without them
//! answers 404, as one from before them does.
use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::{
    friends::parse_friend_code,
    levels::{
        CardAnswers, CardLookup, CardSelector, OwnLevels, Relation, RivalCard,
    },
};
use rusqlite::Transaction;

use crate::{
    AppState,
    auth::Player,
    cards,
    error::{ApiFailure, Result},
    friends::{self, Classes, store},
    http::{Body, GENERAL_BODY, ok},
    routes::{
        play::playing,
        rooms,
    },
    secrets::Keys,
    skill::{self, levels},
};

/// Level requests one player may make per window, both routes together.
const REQUESTS_PER_WINDOW: usize = 60;
const WINDOW_SECS: u64 = 10 * 60;

/// `GET /v1/points/me`. Read only: nothing is remembered as shown.
pub async fn own(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let (player, now) = admit(&state, &headers).await?;
    let keys = state.keys.clone();
    let classes = Classes::of(&state);
    let levels = state
        .db
        .read(move |tx| {
            let mut own = levels::own(tx, &player.ember_id)?;
            if let OwnLevels::Counted { rival, .. } = &mut own {
                if let Some(caller) = skill::account(tx, &player.ember_id)? {
                    *rival = most_played(tx, &keys, &classes, &caller, now)?;
                }
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
    classes: &Classes,
    caller: &str,
    now: u64,
) -> Result<Option<RivalCard>> {
    for pair in levels::pairs_by_games(tx, caller, levels::RIVAL_CANDIDATES)? {
        // A block either way passes the pair over: `relation` is none when
        // they blocked the caller and `Blocked` when the caller blocked them.
        if store::relation(tx, caller, &pair.other)?
            .is_none_or(|relation| relation == Relation::Blocked)
        {
            continue;
        }
        let Some(found) = cards::found(tx, &pair.other)? else {
            continue;
        };
        let Some(card) = cards::issue(keys, &pair.other, now) else {
            continue;
        };
        let rival = RivalCard {
            card,
            name: found.name,
            country: found.country,
            badge: cards::badge(tx, &pair.other, classes, now)?,
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
/// card carries the caller's relation to its player.
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
            let found = lookup
                .cards
                .iter()
                .map(|selector| {
                    let target =
                        target_of(tx, &keys, caller.as_deref(), friends_offered, selector, now)?;
                    match target {
                        Some(target) => cards::card_for(
                            tx,
                            caller.as_deref(),
                            &target,
                            friends_offered,
                            &classes,
                            now,
                        ),
                        None => Ok(None),
                    }
                })
                .collect::<Result<_>>()?;
            Ok(CardAnswers { cards: found })
        })
        .await?;
    Ok(ok(&answers))
}

/// The account `selector` names for `caller`, if it names one they may open.
/// A friend, a request and a code are the caller's own, so they need their
/// Discord account and friends being offered; a code never names the caller.
fn target_of(
    tx: &Transaction<'_>,
    keys: &Keys,
    caller: Option<&str>,
    friends_offered: bool,
    selector: &CardSelector,
    now: u64,
) -> Result<Option<String>> {
    if let CardSelector::Card(token) = selector {
        return Ok(cards::open(keys, token, now));
    }
    let (Some(caller), true) = (caller, friends_offered) else {
        return Ok(None);
    };
    match selector {
        CardSelector::Card(_) => Ok(None),
        CardSelector::Friend(id) => store::friend_of(tx, caller, id),
        CardSelector::Request(id) => store::received_from(tx, caller, id),
        CardSelector::Code(code) => Ok(parse_friend_code(code)
            .map(|code| store::account_of_code(tx, &code))
            .transpose()?
            .flatten()
            .filter(|target| target != caller)),
    }
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
