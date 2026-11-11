//! The fighter level routes (docs/design/FIGHTER_LEVELS.md, "Wire
//! contracts"): a player's own levels and the look-up of other players'
//! cards. Only where public rooms are on, since only their games count; a
//! bridge without them answers 404, as one from before them does.
use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::levels::{CardAnswers, CardLookup, CardSelector};

use crate::{
    AppState,
    auth::Player,
    cards,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    routes::{play::playing, rooms},
    skill::levels,
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

/// `POST /v1/cards/lookup`: one card, or null, per selector, in order.
pub async fn lookup(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let (_, now) = admit(&state, &headers).await?;
    let lookup: CardLookup = body.parse()?;
    lookup.check()?;
    let accounts: Vec<Option<String>> = lookup
        .cards
        .iter()
        .map(|selector| match selector {
            CardSelector::Card(token) => cards::open(&state.keys, token, now),
            // Friends are not offered yet, so no friend id is the caller's
            // friend.
            CardSelector::Friend(_) => None,
        })
        .collect();
    let answers = state
        .db
        .read(move |tx| {
            let cards = accounts
                .iter()
                .map(|account| match account {
                    Some(account) => levels::card(tx, account),
                    None => Ok(None),
                })
                .collect::<Result<_>>()?;
            Ok(CardAnswers { cards })
        })
        .await?;
    Ok(ok(&answers))
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
