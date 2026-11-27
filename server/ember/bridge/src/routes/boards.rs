//! The points boards' route (docs/design/SITE_PLAYER_CARDS.md, "Bridge
//! routes"): any one board for the website, with no credential, and the
//! read of all of them that the signed-in routes take a player's places
//! from (`routes::website`). Only where public rooms are on, since only their
//! games earn points.
use std::sync::{Arc, OnceLock};

use axum::{
    extract::{Query, State, rejection::QueryRejection},
    http::StatusCode,
    response::Response,
};
use ember_protocol::levels::{MAX_POINTS_BOARD, PointsBoard, PointsLine};
use serde::Deserialize;

use crate::{
    AppState, cards,
    error::{ApiFailure, Result},
    friends::Classes,
    http::json_bytes,
    routes::rooms,
    secrets::Keys,
    skill::boards::{Board, Snapshot},
};

/// How long the boards are answered from one read. Anyone can ask for one,
/// and a player's place should not move between two pages of one visit.
const SNAPSHOT_SECS: u64 = 60;

/// The boards as last read, one read at a time.
#[derive(Default)]
pub struct Cache(tokio::sync::Mutex<Option<Arc<Boards>>>);

/// One read of every board, and each board's answer once it was asked for:
/// however often a board is asked for, it is written once a read.
pub struct Boards {
    pub snapshot: Snapshot,
    /// By `Board::slot`.
    answers: Vec<OnceLock<Vec<u8>>>,
}

/// Every board, at most `SNAPSHOT_SECS` old.
pub async fn boards(state: &AppState) -> Result<Arc<Boards>> {
    let now = state.now();
    let mut cached = state.points_boards.0.lock().await;
    if let Some(boards) = cached
        .as_ref()
        .filter(|boards| now.saturating_sub(boards.snapshot.read_at()) < SNAPSHOT_SECS)
    {
        return Ok(Arc::clone(boards));
    }
    let classes = Classes::of(state);
    let snapshot = state
        .db
        .read(move |tx| Snapshot::read(tx, &classes.developers, classes.shown, now))
        .await?;
    let boards = Arc::new(Boards {
        snapshot,
        answers: (0..Board::COUNT).map(|_| OnceLock::new()).collect(),
    });
    *cached = Some(Arc::clone(&boards));
    Ok(boards)
}

/// Which board is asked for: a fighter's, or the account's without one.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Asked {
    fighter: Option<u8>,
}

impl Boards {
    /// The answer for `board`, which `fighter` asked for: its first
    /// `MAX_POINTS_BOARD` lines.
    fn answer(&self, keys: &Keys, board: Board, fighter: Option<u8>) -> &[u8] {
        self.answers[board.slot()].get_or_init(|| {
            let lines = self.snapshot.lines(board);
            let listed_at = self.snapshot.read_at();
            let answer = PointsBoard {
                listed_at,
                fighter,
                total: lines.len(),
                players: lines
                    .iter()
                    .take(MAX_POINTS_BOARD)
                    .map(|line| {
                        let player = self.snapshot.player(line);
                        PointsLine {
                            place: line.place,
                            name: player.listing.name.clone(),
                            country: player.listing.country.clone(),
                            band: player.class.map(|class| class.band),
                            points: line.points,
                            tier: line.tier,
                            card: cards::issue(keys, &player.account, listed_at),
                        }
                    })
                    .collect(),
            };
            serde_json::to_vec(&answer).expect("a board of text and numbers is written as JSON")
        })
    }
}

/// `GET /v1/points/board/public`: the account board, or `fighter`'s. It
/// carries what the class board shows of each player and their points, and no
/// entry is the caller's own.
pub async fn public_board(
    State(state): State<AppState>,
    asked: std::result::Result<Query<Asked>, QueryRejection>,
) -> Result<Response> {
    if !rooms::enabled(&state) {
        return Err(ApiFailure::not_found());
    }
    let asked = asked
        .ok()
        .and_then(|Query(asked)| Some((Board::of(asked.fighter)?, asked.fighter)));
    let Some((board, fighter)) = asked else {
        return Err(ApiFailure::invalid(
            "fighter must be 0 to 43, or left out for the account board",
        ));
    };
    let boards = boards(&state).await?;
    let answer = boards.answer(&state.keys, board, fighter).to_vec();
    Ok(json_bytes(StatusCode::OK, answer))
}
