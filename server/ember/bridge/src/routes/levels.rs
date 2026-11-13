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
    levels::{CardAnswers, CardLookup, CardSelector, PlayerCard, Relation, Versus},
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
        rooms::{self, classes_shown, developers},
    },
    secrets::Keys,
    skill::{self, levels},
};

/// Level requests one player may make per window, both routes together.
const REQUESTS_PER_WINDOW: usize = 60;
const WINDOW_SECS: u64 = 10 * 60;

/// `GET /v1/points/me`. Read only: nothing is remembered as shown.
pub async fn own(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let (player, _) = admit(&state, &headers).await?;
    let levels = state
        .db
        .read(move |tx| levels::own(tx, &player.ember_id))
        .await?;
    Ok(ok(&levels))
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
    let classes = Classes {
        shown: classes_shown(&state),
        developers: developers(&state),
    };
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
                        Some(target) => card_for(
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

/// `target`'s card as `caller` sees it: none when `target` blocked them, with
/// the points, what the player chose to show, their class while classes are
/// shown and their record against the caller. Their relation, and the id of
/// the friendship when it is one, come once friends are offered.
fn card_for(
    tx: &Transaction<'_>,
    caller: Option<&str>,
    target: &str,
    friends_offered: bool,
    classes: &Classes,
    now: u64,
) -> Result<Option<PlayerCard>> {
    let relation = match caller {
        Some(caller) => store::relation(tx, caller, target)?,
        None => Some(Relation::None),
    };
    let Some(relation) = relation else {
        return Ok(None);
    };
    let Some(card) = levels::card(tx, target)? else {
        return Ok(None);
    };
    let relation = if friends_offered {
        relation
    } else {
        Relation::None
    };
    let mut card = PlayerCard {
        marks: levels::card_marks(tx, target)?,
        relation,
        badge: cards::badge(tx, target, classes, now)?,
        ..card
    };
    if let Some(stored) = cards::stored(tx, target)? {
        card = cards::describe(card, stored);
    }
    if let Some(caller) = caller.filter(|caller| *caller != target) {
        card.versus = store::record(tx, caller, target)?.map(|record| Versus {
            won: record.won,
            lost: record.lost,
            last_at: record.last_at,
        });
        if relation == Relation::Friend {
            card.friend = store::friendship_id(tx, caller, target)?;
        }
    }
    Ok(Some(card))
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
