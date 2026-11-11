//! The fighter level routes (docs/design/FIGHTER_LEVELS.md, "Wire
//! contracts"): a player's own levels and the look-up of other players'
//! cards, which friends extend with a friend's card and the caller's relation
//! to each player (docs/design/FRIENDS.md, "The card look-up"). Only where
//! public rooms are on, since only their games count; a bridge without them
//! answers 404, as one from before them does.
use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::levels::{CardAnswers, CardLookup, CardSelector, PlayerCard, Relation};
use rusqlite::Transaction;

use crate::{
    AppState,
    auth::Player,
    cards,
    error::{ApiFailure, Result},
    friends::{self, store},
    http::{Body, GENERAL_BODY, ok},
    routes::{play::playing, rooms},
    skill::{self, levels},
};

/// Level requests one player may make per window, both routes together.
const REQUESTS_PER_WINDOW: usize = 60;
const WINDOW_SECS: u64 = 10 * 60;

/// `GET /v1/levels/me`. Read only: nothing is remembered as shown.
pub async fn own(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let (player, _) = admit(&state, &headers).await?;
    let levels = state
        .db
        .read(move |tx| levels::own(tx, &player.ember_id))
        .await?;
    Ok(ok(&levels))
}

/// `POST /v1/cards/lookup`: one card, or null, per selector, in order. A
/// friend id names the caller's friend, once friends are offered. A player
/// who blocked the caller shows them no card, and once friends are offered
/// each card carries the caller's relation to its player.
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
    let answers = state
        .db
        .read(move |tx| {
            let caller = skill::account(tx, &player.ember_id)?;
            let found = lookup
                .cards
                .iter()
                .map(|selector| {
                    let target = match (selector, &caller) {
                        (CardSelector::Card(token), _) => cards::open(&keys, token, now),
                        (CardSelector::Friend(id), Some(caller)) if friends_offered => {
                            store::friend_of(tx, caller, id)?
                        }
                        (CardSelector::Friend(_), _) => None,
                    };
                    match target {
                        Some(target) => card_for(tx, caller.as_deref(), &target, friends_offered),
                        None => Ok(None),
                    }
                })
                .collect::<Result<_>>()?;
            Ok(CardAnswers { cards: found })
        })
        .await?;
    Ok(ok(&answers))
}

/// `target`'s card as `caller` sees it: none when `target` blocked them, and
/// with their relation once friends are offered.
fn card_for(
    tx: &Transaction<'_>,
    caller: Option<&str>,
    target: &str,
    friends_offered: bool,
) -> Result<Option<PlayerCard>> {
    let relation = match caller {
        Some(caller) => store::relation(tx, caller, target)?,
        None => Some(Relation::None),
    };
    let Some(relation) = relation else {
        return Ok(None);
    };
    let card = levels::card(tx, target)?;
    Ok(card.map(|card| PlayerCard {
        relation: if friends_offered {
            relation
        } else {
            Relation::None
        },
        ..card
    }))
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
            &format!("levels:{}", player.ember_id.as_str()),
            REQUESTS_PER_WINDOW,
            WINDOW_SECS,
            now,
        )
        .map_err(ApiFailure::rate_limited)?;
    Ok((player, now))
}
