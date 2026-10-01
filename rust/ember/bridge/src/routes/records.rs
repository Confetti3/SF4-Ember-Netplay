//! Player records and lobby standings (an extension beyond EMBER-TB-001).
//!
//! Both are computed from finished matches and their accepted games when they
//! are read, so a correction shows at once and no stored counter can drift.
use std::collections::BTreeMap;

use axum::{
    extract::{Path, Query, State},
    http::HeaderMap,
    response::Response,
};
use ember_protocol::EmberId;
use rusqlite::{Transaction, params};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::{
    AppState, auth,
    error::{ApiFailure, Result},
    events::{self, Viewer},
    http::ok,
    routes::matches::viewer_of,
};

const DEFAULT_RECENT: usize = 10;
const MAX_RECENT: usize = 50;

/// One finished match from one player's side.
struct Played {
    match_id: String,
    finished: bool,
    games_to_win: u8,
    lobby_id: Option<String>,
    round_label: Option<String>,
    updated_at: u64,
    opponent: Option<(String, String)>,
    /// Accepted games won by this player, by the opponent, and drawn.
    games: [u32; 3],
}

impl Played {
    fn won(&self) -> bool {
        self.finished && self.games[0] >= u32::from(self.games_to_win)
    }

    fn lost(&self) -> bool {
        self.finished && self.games[1] >= u32::from(self.games_to_win)
    }
}

/// Completed and cancelled matches of `ember_id` that `viewer` may see, newest first.
fn played(tx: &Transaction<'_>, viewer: &Viewer, ember_id: &EmberId) -> Result<Vec<Played>> {
    let (connection, tenant) = match viewer {
        Viewer::Provider { connection_id, .. } => (Some(connection_id.as_str()), None),
        Viewer::Organizer { tenant_id } => (None, Some(tenant_id.as_str())),
        Viewer::Player { .. } => (None, None),
    };
    tx.prepare(
        "SELECT m.id, m.state, m.games_to_win, m.lobby_id, m.metadata, m.updated_at, p.slot,
                o.participant_id, o.ember_id,
                (SELECT COUNT(*) FROM attempts a WHERE a.match_id = m.id AND a.state = 'accepted' AND a.outcome = 'p1_win'),
                (SELECT COUNT(*) FROM attempts a WHERE a.match_id = m.id AND a.state = 'accepted' AND a.outcome = 'p2_win'),
                (SELECT COUNT(*) FROM attempts a WHERE a.match_id = m.id AND a.state = 'accepted' AND a.outcome = 'draw')
         FROM match_participants p
         JOIN matches m ON m.id = p.match_id AND m.assignment_generation = p.assignment_generation
         LEFT JOIN match_participants o
           ON o.match_id = m.id AND o.assignment_generation = m.assignment_generation AND o.slot != p.slot
         WHERE p.ember_id = ?1 AND m.state IN ('completed', 'cancelled')
           AND (?2 IS NULL OR m.connection_id = ?2) AND (?3 IS NULL OR m.tenant_id = ?3)
         ORDER BY m.updated_at DESC, m.rowid DESC",
    )?
    .query_map(params![ember_id.as_str(), connection, tenant], |row| {
        let slot: u8 = row.get(6)?;
        let p1: u32 = row.get(9)?;
        let p2: u32 = row.get(10)?;
        let metadata: String = row.get(4)?;
        let opponent = match (row.get::<_, Option<String>>(7)?, row.get::<_, Option<String>>(8)?) {
            (Some(participant), Some(ember)) => Some((participant, ember)),
            _ => None,
        };
        Ok(Played {
            match_id: row.get(0)?,
            finished: row.get::<_, String>(1)? == "completed",
            games_to_win: row.get(2)?,
            lobby_id: row.get(3)?,
            round_label: serde_json::from_str::<BTreeMap<String, String>>(&metadata)
                .ok()
                .and_then(|mut metadata| metadata.remove("round_label")),
            updated_at: row.get(5)?,
            opponent,
            games: if slot == 0 { [p1, p2, row.get(11)?] } else { [p2, p1, row.get(11)?] },
        })
    })?
    .collect::<rusqlite::Result<Vec<_>>>()
    .map_err(Into::into)
}

#[derive(Default, Serialize)]
struct Tally {
    played: u32,
    won: u32,
    lost: u32,
}

#[derive(Deserialize)]
pub struct RecordQuery {
    #[serde(default)]
    limit: Option<usize>,
}

/// `GET /v1/players/{ember_id}/record`: sets and games won and lost, and the
/// most recent finished matches. A provider sees its connection's matches, an
/// organizer its tenant's, and a player only their own record.
pub async fn record(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(ember_id): Path<String>,
    Query(query): Query<RecordQuery>,
) -> Result<Response> {
    let viewer = viewer_of(&auth::any(&state, &headers).await?)?;
    let ember_id = EmberId::parse(&ember_id).map_err(|_| ApiFailure::not_found())?;
    if let Viewer::Player { ember_id: own } = &viewer
        && *own != ember_id
    {
        return Err(ApiFailure::not_found());
    }
    let limit = query.limit.unwrap_or(DEFAULT_RECENT).min(MAX_RECENT);
    let service = !matches!(viewer, Viewer::Player { .. });
    let body = state
        .db
        .read(move |tx| {
            let rows = played(tx, &viewer, &ember_id)?;
            let mut sets = Tally::default();
            let (mut won, mut lost, mut drawn) = (0u32, 0u32, 0u32);
            let mut cancelled = 0u32;
            for row in &rows {
                if row.finished {
                    sets.played += 1;
                    sets.won += u32::from(row.won());
                    sets.lost += u32::from(row.lost());
                    won += row.games[0];
                    lost += row.games[1];
                    drawn += row.games[2];
                } else {
                    cancelled += 1;
                }
            }
            let recent: Vec<_> = rows
                .iter()
                .take(limit)
                .map(|row| {
                    let result = if !row.finished {
                        "cancelled"
                    } else if row.won() {
                        "won"
                    } else {
                        "lost"
                    };
                    let opponent = row.opponent.as_ref().map(|(participant, ember)| {
                        if service {
                            json!({ "ember_id": ember, "participant_id": participant })
                        } else {
                            json!({ "ember_id": ember })
                        }
                    });
                    json!({
                        "match_id": row.match_id,
                        "result": result,
                        "wins": row.games[0],
                        "opponent_wins": row.games[1],
                        "games_to_win": row.games_to_win,
                        "opponent": opponent,
                        "lobby_id": row.lobby_id,
                        "round_label": row.round_label,
                        "finished_at": row.updated_at,
                    })
                })
                .collect();
            Ok(json!({
                "ember_id": ember_id,
                "sets": sets,
                "games": { "won": won, "lost": lost, "drawn": drawn },
                "cancelled": cancelled,
                "recent": recent,
                "event_cursor": events::head(tx)?.to_string(),
            }))
        })
        .await?;
    Ok(ok(&body))
}

#[derive(Default)]
struct Standing {
    participant_id: String,
    sets_won: u32,
    sets_lost: u32,
    games_won: u32,
    games_lost: u32,
    best_streak: u32,
}

/// Everyone who has finished a set in the lobby: sets won, then fewest lost,
/// then game difference, then best streak.
pub fn lobby_standings(tx: &Transaction<'_>, lobby_id: &str) -> Result<Vec<serde_json::Value>> {
    let mut table: BTreeMap<String, Standing> = BTreeMap::new();
    let rows = tx
        .prepare(
            "SELECT m.games_to_win, p.slot, p.ember_id, p.participant_id,
                    (SELECT COUNT(*) FROM attempts a WHERE a.match_id = m.id AND a.state = 'accepted' AND a.outcome = 'p1_win'),
                    (SELECT COUNT(*) FROM attempts a WHERE a.match_id = m.id AND a.state = 'accepted' AND a.outcome = 'p2_win')
             FROM matches m
             JOIN match_participants p ON p.match_id = m.id AND p.assignment_generation = m.assignment_generation
             WHERE m.lobby_id = ?1 AND m.state = 'completed'",
        )?
        .query_map([lobby_id], |row| {
            Ok((
                row.get::<_, u32>(0)?,
                row.get::<_, u8>(1)?,
                row.get::<_, String>(2)?,
                row.get::<_, String>(3)?,
                row.get::<_, u32>(4)?,
                row.get::<_, u32>(5)?,
            ))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for (goal, slot, ember_id, participant_id, p1, p2) in rows {
        let (mine, theirs) = if slot == 0 { (p1, p2) } else { (p2, p1) };
        let entry = table.entry(ember_id).or_default();
        entry.participant_id = participant_id;
        entry.sets_won += u32::from(mine >= goal);
        entry.sets_lost += u32::from(theirs >= goal);
        entry.games_won += mine;
        entry.games_lost += theirs;
    }
    let streaks = tx
        .prepare("SELECT ember_id, best_streak FROM lobby_entries WHERE lobby_id = ?1")?
        .query_map([lobby_id], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, u32>(1)?))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for (ember_id, best) in streaks {
        if let Some(entry) = table.get_mut(&ember_id) {
            entry.best_streak = best;
        }
    }
    let mut ranked: Vec<_> = table.into_iter().collect();
    ranked.sort_by(|(a_id, a), (b_id, b)| {
        b.sets_won
            .cmp(&a.sets_won)
            .then(a.sets_lost.cmp(&b.sets_lost))
            .then(
                (i64::from(b.games_won) - i64::from(b.games_lost))
                    .cmp(&(i64::from(a.games_won) - i64::from(a.games_lost))),
            )
            .then(b.best_streak.cmp(&a.best_streak))
            .then(a_id.cmp(b_id))
    });
    Ok(ranked
        .into_iter()
        .map(|(ember_id, entry)| {
            json!({
                "ember_id": ember_id,
                "participant_id": entry.participant_id,
                "sets_won": entry.sets_won,
                "sets_lost": entry.sets_lost,
                "games_won": entry.games_won,
                "games_lost": entry.games_lost,
                "best_streak": entry.best_streak,
            })
        })
        .collect())
}
