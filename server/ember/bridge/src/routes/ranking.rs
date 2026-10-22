//! The ranking routes (docs/design/SKILL_BADGE.md, "Rankings"): a player's
//! own standing, their choice to take part, and the board, for players and
//! (the board alone) for the website. Only where public rooms are on, since
//! only their games are rated.
use std::sync::Arc;

use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::skill::{Board, BoardEntry, RankingChoice};

use crate::{
    AppState,
    auth::Player,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    routes::{
        play::playing,
        rooms::{self, classes_shown, developers},
    },
    skill::ranking,
};

/// Ranking requests one player may make per window, all routes together.
const REQUESTS_PER_WINDOW: usize = 60;
const WINDOW_SECS: u64 = 10 * 60;

/// `GET /v1/ranking/me`. A write: a class shown is remembered as shown,
/// as a ticket's is.
pub async fn standing(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let (player, now) = admit(&state, &headers).await?;
    let (shown, developers) = (classes_shown(&state), developers(&state));
    let standing = state
        .db
        .write(move |tx| ranking::standing(tx, &player.ember_id, shown, &developers, now))
        .await?;
    Ok(ok(&standing))
}

/// `PUT /v1/ranking/me`, answered with the standing after it.
pub async fn choose(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let (player, now) = admit(&state, &headers).await?;
    let chosen: RankingChoice = body.parse()?;
    chosen.check()?;
    let (shown, developers) = (classes_shown(&state), developers(&state));
    let standing = state
        .db
        .write(move |tx| ranking::choose(tx, &player.ember_id, chosen, shown, &developers, now))
        .await?;
    Ok(ok(&standing))
}

/// `GET /v1/ranking/board`: empty while classes are hidden.
pub async fn board(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let (player, now) = admit(&state, &headers).await?;
    if !classes_shown(&state) {
        return Ok(ok(&Board {
            players: Vec::new(),
        }));
    }
    let developers = developers(&state);
    let board = state
        .db
        .read(move |tx| ranking::board(tx, Some(&player.ember_id), &developers, now))
        .await?;
    Ok(ok(&board))
}

/// How long a public board is answered again before it is read again.
const PUBLIC_BOARD_SECS: u64 = 60;

/// The board on the embernetplay.link rankings page. `shown` is false while
/// classes are hidden, so the page can tell that from a board nobody is on yet.
#[derive(serde::Serialize)]
struct PublicBoard {
    listed_at: u64,
    shown: bool,
    players: Vec<BoardEntry>,
}

/// The last public board. Anyone can ask for it, directly or through the
/// website, so the bridge reads it at most once a minute, one read at a time.
#[derive(Default)]
pub struct PublicCache(tokio::sync::Mutex<Option<Arc<PublicBoard>>>);

/// `GET /v1/ranking/board/public`: the board as players see it in Ember, for a
/// web page, with no credential. It carries what the board shows any Ember ID,
/// the names, flags and classes players chose to be listed with, and no entry
/// is the caller's own.
pub async fn public_board(State(state): State<AppState>) -> Result<Response> {
    if !rooms::enabled(&state) {
        return Err(ApiFailure::not_found());
    }
    let now = state.now();
    let shown = classes_shown(&state);
    let mut cached = state.ranking_board.0.lock().await;
    if let Some(board) = cached.as_ref().filter(|board| {
        board.shown == shown && now.saturating_sub(board.listed_at) < PUBLIC_BOARD_SECS
    }) {
        return Ok(ok(&**board));
    }
    let players = if shown {
        let developers = developers(&state);
        state
            .db
            .read(move |tx| ranking::board(tx, None, &developers, now))
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
    *cached = Some(Arc::clone(&board));
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
