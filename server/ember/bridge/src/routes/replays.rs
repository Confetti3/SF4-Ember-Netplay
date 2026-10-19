//! A set's replays and its pack (docs/design/TOURNAMENT_DESK.md, 4b).
//!
//! Each of the set's two players sends every game whose result was
//! confirmed, as the `.emberreplay` the game archived; the same game from
//! both is kept once, by its CRC. The pack is the set's games in save-time
//! order behind one header.
use axum::{
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode, header},
    response::{IntoResponse, Response},
};
use ember_protocol::replays;
use rusqlite::{OptionalExtension, params};
use serde::Deserialize;
use serde_json::json;

use crate::{
    AppState, auth,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, json as respond},
    routes::{
        ledger::{load, participants, scores},
        matches::{viewer_of, visible},
        play::{fighter, playing},
    },
};

/// More games than any set plays, draws included.
const MAX_GAMES: i64 = 32;

#[derive(Deserialize)]
pub struct Sent {
    /// The room binding's revision the game was played under.
    revision: u64,
}

/// `PUT /v1/matches/{id}/replays/{crc}?revision=N`: one of the set's two
/// players, with their play session, sends a game's `.emberreplay`. 201 when
/// it is new, 200 when the set already holds that game.
pub async fn put(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((id, crc)): Path<(String, String)>,
    Query(sent): Query<Sent>,
    body: Body<{ replays::MAX_REPLAY }>,
) -> Result<Response> {
    let player = playing(&state, &headers).await?;
    let replay = body.0;
    let saved_at = replays::saved_at(&crc, &replay)
        .ok_or_else(|| ApiFailure::invalid("That is not a whole replay with this CRC."))?;
    let ctx = Ctx::of(&state);
    let status = state
        .db
        .write(move |tx| {
            fighter(tx, &id, &player.ember_id)?;
            let (held, kept): (i64, bool) = tx.query_row(
                "SELECT COUNT(*), COALESCE(SUM(crc = ?2), 0) > 0 FROM match_replays WHERE match_id = ?1",
                params![id, crc],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?;
            if kept {
                return Ok(StatusCode::OK);
            }
            if held >= MAX_GAMES {
                return Err(ApiFailure::invalid("This set holds all the replays it can."));
            }
            tx.execute(
                "INSERT INTO match_replays (match_id, crc, revision, saved_at, sent_by, replay, received_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
                params![id, crc, sent.revision, saved_at, player.ember_id.as_str(), &replay[..], ctx.now],
            )?;
            Ok(StatusCode::CREATED)
        })
        .await?;
    Ok(respond(status, &json!({})))
}

/// `GET /v1/matches/{id}/replays.pack`: anyone who may see the match gets
/// its pack (`replays::PACK_MAGIC`). The header says the tournament, round,
/// phase, players, score, the number of rooms played in, and each game's
/// CRC, revision, save time and size, in save-time order.
pub async fn pack(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let viewer = viewer_of(&auth::any(&state, &headers).await?)?;
    let file_name = format!("{id}.emberpack");
    let bytes = state
        .db
        .read(move |tx| {
            let found = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &found)? {
                return Err(ApiFailure::not_found());
            }
            let (wins, _) = scores(tx, &found.id)?;
            let metadata: String = tx.query_row(
                "SELECT metadata FROM matches WHERE id = ?1",
                [&found.id],
                |row| row.get(0),
            )?;
            let metadata: serde_json::Value = serde_json::from_str(&metadata).unwrap_or_default();
            // A bracket set's external ID is `<site>:<tournament>:<set>:<attempt>`;
            // its desk event names the tournament.
            let tournament: Option<String> = tx
                .query_row(
                    "SELECT title FROM organizer_events WHERE tournament IS NOT NULL
                       AND substr(?1, 1, length(site) + length(tournament) + 2) = site || ':' || tournament || ':'
                     ORDER BY created_at DESC LIMIT 1",
                    [&found.external_match_id],
                    |row| row.get(0),
                )
                .optional()?;
            let games = tx
                .prepare(
                    "SELECT crc, revision, saved_at, replay FROM match_replays
                     WHERE match_id = ?1 ORDER BY saved_at, received_at",
                )?
                .query_map([&found.id], |row| {
                    Ok((
                        row.get::<_, String>(0)?,
                        row.get::<_, u64>(1)?,
                        row.get::<_, u64>(2)?,
                        row.get::<_, Vec<u8>>(3)?,
                    ))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            let mut rooms: Vec<u64> = games.iter().map(|game| game.1).collect();
            rooms.sort_unstable();
            rooms.dedup();
            let header = serde_json::to_vec(&json!({
                "match_id": found.id,
                "tournament": tournament,
                "round_label": metadata.get("round_label"),
                "round": metadata.get("round"),
                "phase": metadata.get("phase"),
                "players": participants(tx, &found.id, found.generation)?,
                "score": wins,
                "rooms": rooms.len(),
                "games": games.iter().map(|(crc, revision, saved_at, replay)| json!({
                    "crc": crc, "revision": revision, "saved_at": saved_at, "size": replay.len(),
                })).collect::<Vec<_>>(),
            }))
            .map_err(|_| ApiFailure::unavailable())?;
            let mut out = replays::PACK_MAGIC.to_vec();
            out.extend_from_slice(&(header.len() as u32).to_le_bytes());
            out.extend_from_slice(&header);
            for (_, _, _, replay) in &games {
                out.extend_from_slice(replay);
            }
            Ok(out)
        })
        .await?;
    Ok((
        [
            (header::CONTENT_TYPE, "application/octet-stream".to_owned()),
            (
                header::CONTENT_DISPOSITION,
                format!("attachment; filename=\"{file_name}\""),
            ),
        ],
        bytes,
    )
        .into_response())
}
