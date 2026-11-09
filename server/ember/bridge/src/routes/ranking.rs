//! The ranking routes (docs/design/SKILL_BADGE.md, "Rankings"): a player's
//! own standing, their choice to take part, and the board, for players and
//! (the board alone) for the website. Only where public rooms are on, since
//! only their games are rated.
//!
//! Each route is one game's (`?game=`, a key of `ember_protocol::rooms::GAMES`;
//! SF4's when absent, so a request that predates games is answered as it
//! was): SFV Ember asks for its own, and the website for each in turn.
use std::{collections::HashMap, sync::Arc};

use axum::{
    extract::{Query, State},
    http::HeaderMap,
    response::Response,
};
use ember_protocol::skill::{Board, BoardEntry, RankingChoice};

use crate::{
    AppState,
    auth::Player,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    routes::{
        play::playing,
        rooms::{self, GameQuery, classes_shown, developers, listed_game},
    },
    skill::ranking,
};

/// Ranking requests one player may make per window, all routes together.
const REQUESTS_PER_WINDOW: usize = 60;
const WINDOW_SECS: u64 = 10 * 60;

/// `GET /v1/ranking/me?game=`. A write: a class shown is remembered as
/// shown, as a ticket's is.
pub async fn standing(
    State(state): State<AppState>,
    Query(query): Query<GameQuery>,
    headers: HeaderMap,
) -> Result<Response> {
    let (player, now) = admit(&state, &headers).await?;
    let game = listed_game(query.game.as_deref())?;
    let (shown, developers) = (classes_shown(&state), developers(&state));
    let standing = state
        .db
        .write(move |tx| ranking::standing(tx, &game, &player.ember_id, shown, &developers, now))
        .await?;
    Ok(ok(&standing))
}

/// `PUT /v1/ranking/me?game=`, answered with the standing after it.
pub async fn choose(
    State(state): State<AppState>,
    Query(query): Query<GameQuery>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let (player, now) = admit(&state, &headers).await?;
    let game = listed_game(query.game.as_deref())?;
    let chosen: RankingChoice = body.parse()?;
    chosen.check()?;
    let (shown, developers) = (classes_shown(&state), developers(&state));
    let standing = state
        .db
        .write(move |tx| {
            ranking::choose(tx, &game, &player.ember_id, chosen, shown, &developers, now)
        })
        .await?;
    Ok(ok(&standing))
}

/// `GET /v1/ranking/board?game=`: empty while classes are hidden.
pub async fn board(
    State(state): State<AppState>,
    Query(query): Query<GameQuery>,
    headers: HeaderMap,
) -> Result<Response> {
    let (player, now) = admit(&state, &headers).await?;
    let game = listed_game(query.game.as_deref())?;
    if !classes_shown(&state) {
        return Ok(ok(&Board {
            players: Vec::new(),
        }));
    }
    let developers = developers(&state);
    let board = state
        .db
        .read(move |tx| ranking::board(tx, &game, Some(&player.ember_id), &developers, now))
        .await?;
    Ok(ok(&board))
}

/// How long a public board is answered again before it is read again.
const PUBLIC_BOARD_SECS: u64 = 15;

/// The board on the embernetplay.link rankings page. `shown` is false while
/// classes are hidden, so the page can tell that from a board nobody is on yet.
#[derive(serde::Serialize)]
struct PublicBoard {
    listed_at: u64,
    shown: bool,
    players: Vec<BoardEntry>,
}

/// The last public board of each game. Anyone can ask for one, directly or
/// through the website, so the bridge reads each at most every 15 s, one
/// read at a time.
#[derive(Default)]
pub struct PublicCache(tokio::sync::Mutex<HashMap<String, Arc<PublicBoard>>>);

/// `GET /v1/ranking/board/public?game=`: a game's board as players see it in
/// Ember, for a web page, with no credential. It carries what the board shows
/// any Ember ID, the names, flags and classes players chose to be listed
/// with, and no entry is the caller's own.
pub async fn public_board(
    State(state): State<AppState>,
    Query(query): Query<GameQuery>,
) -> Result<Response> {
    if !rooms::enabled(&state) {
        return Err(ApiFailure::not_found());
    }
    let game = listed_game(query.game.as_deref())?;
    let now = state.now();
    let shown = classes_shown(&state);
    let mut cached = state.ranking_board.0.lock().await;
    if let Some(board) = cached.get(&game).filter(|board| {
        board.shown == shown && now.saturating_sub(board.listed_at) < PUBLIC_BOARD_SECS
    }) {
        return Ok(ok(&**board));
    }
    let players = if shown {
        let developers = developers(&state);
        let read = game.clone();
        state
            .db
            .read(move |tx| ranking::board(tx, &read, None, &developers, now))
            .await?
            .players
    } else {
        Vec::new()
    };
    let board = Arc::new(PublicBoard {
        listed_at: now,
        shown,
        players,
    });
    cached.insert(game, Arc::clone(&board));
    Ok(ok(&*board))
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
            &format!("ranking:{}", player.ember_id.as_str()),
            REQUESTS_PER_WINDOW,
            WINDOW_SECS,
            now,
        )
        .map_err(ApiFailure::rate_limited)?;
    Ok((player, now))
}
