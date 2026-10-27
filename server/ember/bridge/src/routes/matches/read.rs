//! Reading matches: who may see one, its snapshot, and a player's
//! assignments.
use axum::{
    extract::{Path, State},
    http::HeaderMap,
    response::Response,
};
use ember_protocol::{EmberId, matches::MatchState, play::play_url};
use rusqlite::{Transaction, params};
use serde_json::json;

use crate::{
    AppState, auth,
    error::{ApiFailure, Result},
    events::{self, Viewer},
    http::ok,
    routes::{
        expiry::EXPIRED_LISTED_SECS,
        ledger::{Match, P1_GAMES, P2_GAMES, load, participants, scores},
    },
};

/// Who may see a match: its assigned players, its provider connection, or
/// an organizer of its tenant. Anyone else gets `not_found`.
pub fn visible(tx: &Transaction<'_>, actor: &Viewer, found: &Match) -> Result<bool> {
    Ok(match actor {
        Viewer::Player { ember_id } => tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM match_participants WHERE match_id = ?1 AND ember_id = ?2)",
            params![found.id, ember_id.as_str()],
            |row| row.get(0),
        )?,
        service => service.sees(&found.tenant_id, &found.connection_id),
    })
}

/// A consistent snapshot plus the event cursor from the same read (17.3).
fn snapshot(tx: &Transaction<'_>, bridge_id: &str, found: &Match) -> Result<serde_json::Value> {
    let roster = participants(tx, &found.id, found.generation)?;
    let (wins, _) = scores(tx, &found.id)?;
    let (rules, metadata, created_at, expires_at): (String, String, u64, Option<u64>) = tx
        .query_row(
            "SELECT m.rules, m.metadata, m.created_at, m.expires_at FROM matches m WHERE m.id = ?1",
            [&found.id],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
        )?;
    let attempts = tx
        .prepare(
            "SELECT id, seq, outcome, state, source, adjudication_id FROM attempts WHERE match_id = ?1 ORDER BY seq",
        )?
        .query_map([&found.id], |row| {
            Ok(json!({
                "attempt_id": row.get::<_, String>(0)?,
                "seq": row.get::<_, u64>(1)?,
                "outcome": row.get::<_, Option<String>>(2)?,
                "state": row.get::<_, String>(3)?,
                "source": row.get::<_, String>(4)?,
                "adjudication_id": row.get::<_, Option<String>>(5)?,
            }))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let scores: Vec<_> = roster
        .iter()
        .map(|p| json!({ "ember_id": p.ember_id, "wins": wins[usize::from(p.slot)] }))
        .collect();
    Ok(json!({
        "match_id": found.id,
        "external_match_id": found.external_match_id,
        "state": found.state,
        "revision": found.revision.to_string(),
        "play_url": play_url(bridge_id, &found.id),
        "assignment_generation": found.generation.to_string(),
        "games_to_win": found.games_to_win,
        "rules": serde_json::from_str::<serde_json::Value>(&rules).unwrap_or_default(),
        "participants": roster,
        "scores": scores,
        "attempts": attempts,
        "provider_delivery_state": found.delivery,
        "created_at": created_at,
        "expires_at": expires_at,
        "metadata": serde_json::from_str::<serde_json::Value>(&metadata).unwrap_or_default(),
        "event_cursor": events::head(tx)?.to_string(),
    }))
}

/// `GET /v1/matches/{id}`.
pub async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let viewer = Viewer::of(&auth::any(&state, &headers).await?)?;
    let bridge_id = state.config.bridge_id.clone();
    let body = state
        .db
        .read(move |tx| {
            let found = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &found)? {
                return Err(ApiFailure::not_found());
            }
            snapshot(tx, &bridge_id, &found)
        })
        .await?;
    Ok(ok(&body))
}

/// `GET /v1/assignments`: the caller's own matches only, with what a player
/// needs to pick one: the set length and score, the round, the opponent's
/// fingerprint and whether the game is played through Ember. Each row has the
/// match's `created_at` and `expires_at` (unix seconds; `expires_at` is null
/// for a lobby or bracket set). An expired match stays listed, as `expired`,
/// for a day.
pub async fn assignments(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let config = state.config.clone();
    let listed_since = state.now().saturating_sub(EXPIRED_LISTED_SECS);
    let body = state
        .db
        .read(move |tx| {
            let rows = tx
                .prepare(&format!(
                    "SELECT m.id, m.connection_id, m.state, m.revision, p.slot, m.games_to_win, m.rules, m.metadata,
                            o.ember_id, m.created_at, m.expires_at,
                            m.starting_wins_p1 + {P1_GAMES}, m.starting_wins_p2 + {P2_GAMES}
                     FROM matches m
                     JOIN match_participants p ON p.match_id = m.id AND p.assignment_generation = m.assignment_generation
                     JOIN match_participants o ON o.match_id = m.id AND o.assignment_generation = m.assignment_generation
                       AND o.slot != p.slot
                     WHERE p.ember_id = ?1 AND (m.state != 'expired' OR m.updated_at > ?2)
                     ORDER BY (m.state IN ('completed', 'cancelled', 'failed', 'expired')), m.updated_at DESC
                     LIMIT 50"
                ))?
                .query_map(params![player.ember_id.as_str(), listed_since], |row| {
                    Ok((
                        row.get::<_, String>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, String>(2)?,
                        row.get::<_, u64>(3)?,
                        row.get::<_, u8>(4)?,
                        row.get::<_, u8>(5)?,
                        row.get::<_, String>(6)?,
                        row.get::<_, String>(7)?,
                        row.get::<_, String>(8)?,
                        row.get::<_, u64>(9)?,
                        row.get::<_, Option<u64>>(10)?,
                        [row.get::<_, u8>(11)?, row.get::<_, u8>(12)?],
                    ))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            let mut assignments = Vec::with_capacity(rows.len());
            for (id, connection, state, revision, slot, games_to_win, rules, metadata, opponent, created_at, expires_at, wins) in rows {
                // A state this bridge does not know (a newer bridge wrote it
                // before a rollback) is left out rather than shown.
                if MatchState::parse(&state).is_none() {
                    eprintln!("ember-bridge: match {id} has a stored state this bridge does not know; not listed");
                    continue;
                }
                let rules: serde_json::Value = serde_json::from_str(&rules).unwrap_or_default();
                let metadata: serde_json::Value = serde_json::from_str(&metadata).unwrap_or_default();
                let opponent = EmberId::parse(&opponent).map_err(|_| ApiFailure::unavailable())?;
                assignments.push(json!({
                    "match_id": id,
                    "provider": config.connection(&connection).map(|(_, c)| c.display_name.clone()),
                    "state": state,
                    "revision": revision.to_string(),
                    "slot": slot,
                    "games_to_win": games_to_win,
                    "native_rules_profile": rules.get("native_rules_profile"),
                    "round_label": metadata.get("round_label"),
                    "opponent": { "ember_id": opponent, "fingerprint": opponent.fingerprint() },
                    "wins": wins,
                    "created_at": created_at,
                    "expires_at": expires_at,
                }));
            }
            Ok(json!({ "assignments": assignments, "event_cursor": events::head(tx)?.to_string() }))
        })
        .await?;
    Ok(ok(&body))
}
