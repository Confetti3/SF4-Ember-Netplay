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
use ember_protocol::{matches::MatchState, replays};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Deserialize;
use serde_json::json;

use crate::{
    AppState, auth,
    ctx::Ctx,
    error::{ApiFailure, Result},
    events::Viewer,
    http::{Body, json as respond},
    routes::{
        archive,
        ledger::{Match, load, participants, scores},
        matches::visible,
        play::{fighter, playing},
    },
};

/// What every set's replays together may take on the bridge.
// ponytail: summed over the table on each new replay; keep a running total
// if bridges come to hold many thousands of sets.
const MAX_STORED: i64 = 2 << 30;

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
    Query(sides): Query<replays::Sides>,
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
                // The other player's copy still adds their side's country.
                archive::keep(tx, &ctx, &crc, &replay, &player.ember_id, &sides, None)?;
                return Ok(StatusCode::OK);
            }
            // One replay for each game the set has played, and no more than
            // the bridge keeps for every set together.
            let (played, stored): (i64, i64) = tx.query_row(
                "SELECT (SELECT COUNT(*) FROM attempts WHERE match_id = ?1),
                        (SELECT COALESCE(SUM(length(replay)), 0) FROM match_replays)",
                [&id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?;
            if held >= played {
                return Err(ApiFailure::invalid("This set holds a replay for each game it played."));
            }
            // Refused for good, so the games stop sending it.
            if stored + replay.len() as i64 > MAX_STORED {
                return Err(ApiFailure::invalid("This bridge keeps no more replays."));
            }
            tx.execute(
                "INSERT INTO match_replays (match_id, crc, revision, saved_at, sent_by, replay, received_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
                params![id, crc, sent.revision, saved_at, player.ember_id.as_str(), &replay[..], ctx.now],
            )?;
            archive::keep(tx, &ctx, &crc, &replay, &player.ember_id, &sides, Some(&id))?;
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
    let viewer = Viewer::of(&auth::any(&state, &headers).await?)?;
    let file_name = format!("{id}.emberpack");
    let bytes = state
        .db
        .read(move |tx| {
            let found = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            if !visible(tx, &viewer, &found)? {
                return Err(ApiFailure::not_found());
            }
            packed(tx, &found)
        })
        .await?;
    Ok(download(bytes, &file_name))
}

/// `GET /v1/public/matches/{id}/replays.pack` and
/// `GET /v1/public/matches/{id}/replays/{crc}`: a completed set's pack, or
/// one of its games, to anyone, when its desk event is public
/// (docs/design/BRACKET_SITE.md, "Replays from the report").
pub async fn public_pack(
    State(state): State<AppState>,
    Path(id): Path<String>,
) -> Result<Response> {
    let file_name = format!("{id}.emberpack");
    let bytes = state
        .db
        .read(move |tx| packed(tx, &public_match(tx, &id)?))
        .await?;
    Ok(download(bytes, &file_name))
}

/// `GET /v1/public/matches/{id}/replays`: the header of what
/// `public_pack` serves, without the games.
pub async fn public_header(
    State(state): State<AppState>,
    Path(id): Path<String>,
) -> Result<Response> {
    let header = state
        .db
        .read(move |tx| Ok(pack_parts(tx, &public_match(tx, &id)?)?.0))
        .await?;
    Ok(respond(StatusCode::OK, &header))
}

pub async fn public_game(
    State(state): State<AppState>,
    Path((id, crc)): Path<(String, String)>,
) -> Result<Response> {
    let file_name = format!("{crc}.emberreplay");
    let bytes = state
        .db
        .read(move |tx| {
            let found = public_match(tx, &id)?;
            tx.query_row(
                "SELECT replay FROM match_replays WHERE match_id = ?1 AND crc = ?2",
                params![found.id, crc],
                |row| row.get::<_, Vec<u8>>(0),
            )
            .optional()?
            .ok_or_else(ApiFailure::not_found)
        })
        .await?;
    Ok(download(bytes, &file_name))
}

/// The match, when it is completed and its desk event is public; else
/// not found, as for a match that does not exist.
fn public_match(tx: &Transaction<'_>, id: &str) -> Result<Match> {
    let found = load(tx, id)?.ok_or_else(ApiFailure::not_found)?;
    // A desk event names its sets' matches in the sets the connector sent.
    let public: bool = tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM organizer_events e, json_each(e.sets) s
           WHERE e.public = 1 AND json_extract(s.value, '$.match_id') = ?1)",
        [&found.id],
        |row| row.get(0),
    )?;
    if !public || found.state != MatchState::Completed {
        return Err(ApiFailure::not_found());
    }
    Ok(found)
}

fn download(bytes: Vec<u8>, file_name: &str) -> Response {
    (
        [
            (header::CONTENT_TYPE, "application/octet-stream".to_owned()),
            (
                header::CONTENT_DISPOSITION,
                format!("attachment; filename=\"{file_name}\""),
            ),
        ],
        bytes,
    )
        .into_response()
}

/// What a set's pack and the archive say of it: its desk event's title, its
/// round and phase, its players, and each one's account name on the event's
/// site, as the desk shows them, so an imported pack's games carry names.
pub struct Described {
    pub tournament: Option<String>,
    pub metadata: serde_json::Value,
    pub players: Vec<crate::routes::ledger::Participant>,
    pub names: Vec<Option<String>>,
}

pub fn described(tx: &Transaction<'_>, found: &Match) -> Result<Described> {
    let metadata: String = tx.query_row(
        "SELECT metadata FROM matches WHERE id = ?1",
        [&found.id],
        |row| row.get(0),
    )?;
    let metadata: serde_json::Value = serde_json::from_str(&metadata).unwrap_or_default();
    // A bracket set's external ID is `<site>:<tournament>:<set>:<attempt>`;
    // its desk event names the tournament. An `http` service's sets are
    // named `http:...`, so its event is found by the sets the connector sent.
    let event: Option<(String, String)> = tx
        .query_row(
            "SELECT title, site FROM organizer_events e WHERE tournament IS NOT NULL
               AND (substr(?1, 1, length(site) + length(tournament) + 2) = site || ':' || tournament || ':'
                 OR EXISTS (SELECT 1 FROM json_each(e.sets) s WHERE json_extract(s.value, '$.match_id') = ?2))
             ORDER BY created_at DESC LIMIT 1",
            [&found.external_match_id, &found.id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?;
    let players = participants(tx, &found.id, found.generation)?;
    let mut names = Vec::new();
    for player in &players {
        names.push(match &event {
            Some((_, site)) => tx
                .query_row(
                    "SELECT username FROM site_accounts WHERE site = ?1 AND ember_id = ?2",
                    params![site, player.ember_id.as_str()],
                    |row| row.get::<_, String>(0),
                )
                .optional()?,
            None => None,
        });
    }
    Ok(Described {
        tournament: event.map(|(title, _)| title),
        metadata,
        players,
        names,
    })
}

/// A set's pack header and its games' bytes, in save-time order.
fn pack_parts(tx: &Transaction<'_>, found: &Match) -> Result<(serde_json::Value, Vec<Vec<u8>>)> {
    let (wins, _) = scores(tx, &found.id)?;
    let Described {
        tournament,
        metadata,
        players,
        names,
    } = described(tx, found)?;
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
    let header = json!({
        "match_id": found.id,
        "tournament": tournament,
        "round_label": metadata.get("round_label"),
        "round": metadata.get("round"),
        "phase": metadata.get("phase"),
        "players": players,
        "names": names,
        "score": wins,
        "rooms": rooms.len(),
        "games": games.iter().map(|(crc, revision, saved_at, replay)| json!({
            "crc": crc, "revision": revision, "saved_at": saved_at, "size": replay.len(),
        })).collect::<Vec<_>>(),
    });
    Ok((
        header,
        games.into_iter().map(|(_, _, _, replay)| replay).collect(),
    ))
}

/// A set's pack, as `pack` serves it.
pub fn packed(tx: &Transaction<'_>, found: &Match) -> Result<Vec<u8>> {
    let (header, games) = pack_parts(tx, found)?;
    let header = serde_json::to_vec(&header).map_err(|_| ApiFailure::unavailable())?;
    let mut out = replays::PACK_MAGIC.to_vec();
    out.extend_from_slice(&(header.len() as u32).to_le_bytes());
    out.extend_from_slice(&header);
    for replay in &games {
        out.extend_from_slice(replay);
    }
    Ok(out)
}

/// Sets pruned per maintenance pass, so a first pass after retention is
/// turned on holds the database only briefly.
const PRUNED_PER_PASS: i64 = 4;

/// Removes the replays of sets that last changed more than
/// `replay_retention.days` ago, after writing each set's pack to the backup
/// folder when one is set. A set whose backup cannot be written keeps its
/// replays and is tried again next pass. Runs with the bridge's maintenance.
pub fn prune(tx: &Transaction<'_>, ctx: &Ctx) -> Result<()> {
    let Some(retention) = &ctx.config.replay_retention else {
        return Ok(());
    };
    let cutoff = ctx.now.saturating_sub(retention.days * 24 * 60 * 60);
    let old = tx
        .prepare(
            "SELECT id FROM matches WHERE updated_at <= ?1
               AND EXISTS (SELECT 1 FROM match_replays WHERE match_id = matches.id)
             ORDER BY updated_at LIMIT ?2",
        )?
        .query_map(params![cutoff, PRUNED_PER_PASS], |row| {
            row.get::<_, String>(0)
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for id in old {
        if let Some(folder) = &retention.backup {
            let found = load(tx, &id)?.ok_or_else(ApiFailure::not_found)?;
            let pack = packed(tx, &found)?;
            let written = std::fs::create_dir_all(folder)
                .and_then(|()| std::fs::write(folder.join(format!("{id}.emberpack")), pack));
            if let Err(error) = written {
                eprintln!("maintenance: could not back up the replays of {id}: {error}");
                continue;
            }
        }
        tx.execute("DELETE FROM match_replays WHERE match_id = ?1", [&id])?;
    }
    Ok(())
}
