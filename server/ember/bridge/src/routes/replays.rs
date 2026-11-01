//! A set's replays and its pack (docs/design/TOURNAMENT_DESK.md, 4b).
//!
//! Each of the set's two players sends every game whose result was
//! confirmed, as the `.emberreplay` the game archived; the same game from
//! both is kept once, by its CRC, and each confirmed game holds one replay. The pack is the set's games in save-time
//! order behind one header.
use axum::{
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode, header},
    response::{IntoResponse, Response},
};
use ember_protocol::{EmberId, api::ErrorCode, matches::MatchState, replays};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Deserialize;
use serde_json::json;

use crate::{
    AppState,
    archive::sets,
    auth,
    ctx::Ctx,
    error::{ApiFailure, Result},
    events::Viewer,
    http::{Body, json as respond},
    routes::{
        ledger::{Match, load, participants, scores},
        matches::visible,
        play::{fighter, playing},
    },
};

/// What every set's replays together may take on the bridge.
// ponytail: summed over the table on each new replay; keep a running total
// if bridges come to hold many thousands of sets.
const MAX_STORED: i64 = 2 << 30;
/// How far a replay's save time may stray outside its game's time: from
/// the game's permit to the report that confirmed it.
const GAME_SLACK: u64 = 120;

#[derive(Deserialize)]
pub struct Sent {
    /// The room binding's revision the game was played under, as its sender
    /// says. The bridge does not record which binding each game was played
    /// under, so it checks only that the match had this revision.
    revision: u64,
}

/// A replay one of a set's players sends, as the route read it.
struct Offered<'a> {
    crc: &'a str,
    saved_at: u32,
    revision: u64,
    sender: &'a EmberId,
    replay: &'a [u8],
}

/// `PUT /v1/matches/{id}/replays/{crc}?revision=N`: one of the set's two
/// players, with their play session, sends the `.emberreplay` of one of its
/// confirmed games. 201 when it is new, 200 when the set already holds that
/// game. Either way the archive gets the set's copy, with the sender's own
/// country. A set whose replays retention removed takes none
/// (`set_closed`).
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
            let found = fighter(tx, &id, &player.ember_id)?.found;
            if closed(tx, &id)? {
                return Err(ApiFailure::invalid(
                    "This set's replays were removed; it takes no more.",
                )
                .detail("reason", "set_closed"));
            }
            let kept: bool = tx.query_row(
                "SELECT EXISTS (SELECT 1 FROM match_replays WHERE match_id = ?1 AND crc = ?2)",
                params![id, crc],
                |row| row.get(0),
            )?;
            if !kept {
                let offered = Offered {
                    crc: &crc,
                    saved_at,
                    revision: sent.revision,
                    sender: &player.ember_id,
                    replay: &replay,
                };
                hold(tx, &ctx, &found, &offered)?;
            }
            // The archive's refusal is for now: a new game is not kept for
            // the set either, and the game sends it again.
            let country = sides.country.as_deref();
            if let Some(refused) = sets::keep(tx, &ctx, &found, &crc, &player.ember_id, country)? {
                return Err(refused.into());
            }
            Ok(if kept {
                StatusCode::OK
            } else {
                StatusCode::CREATED
            })
        })
        .await?;
    Ok(respond(status, &json!({})))
}

/// Whether replay retention removed set `match_id`'s replays, which closes
/// it: a pack is never built from part of a set.
fn closed(tx: &Transaction<'_>, match_id: &str) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM archive_sets WHERE match_id = ?1 AND pruned_at IS NOT NULL)",
        [match_id],
        |row| row.get(0),
    )?)
}

/// Keeps `offered` for set `found` as the replay of the confirmed game it
/// was saved in, when that game has none yet and the bridge has room.
fn hold(tx: &Transaction<'_>, ctx: &Ctx, found: &Match, offered: &Offered<'_>) -> Result<()> {
    let bound: u64 = tx.query_row(
        "SELECT COALESCE((SELECT binding_revision FROM match_rooms WHERE match_id = ?1), 0)",
        [&found.id],
        |row| row.get(0),
    )?;
    if !(1..=bound).contains(&offered.revision) {
        return Err(ApiFailure::invalid(
            "This set had no room with that revision.",
        ));
    }
    let attempt = match free_game(tx, &found.id, offered.saved_at)? {
        Some(attempt) => attempt,
        // The game sends it again later, as it does after a 503.
        None if unconfirmed(tx, &found.id, offered.saved_at, ctx.now)? => {
            return Err(ApiFailure::new(
                ErrorCode::ServiceUnavailable,
                "The game this replay was saved in is not confirmed yet. Send it again later.",
            )
            .detail("reason", "replay_unconfirmed"));
        }
        None => {
            return Err(ApiFailure::invalid(
                "This replay was saved in no game of this set that has none yet.",
            ));
        }
    };
    // Refused for good, so the games stop sending it.
    let stored: i64 = tx.query_row(
        "SELECT COALESCE(SUM(length(replay)), 0) FROM match_replays",
        [],
        |row| row.get(0),
    )?;
    if stored + offered.replay.len() as i64 > MAX_STORED {
        return Err(ApiFailure::invalid("This bridge keeps no more replays."));
    }
    tx.execute(
        "INSERT INTO match_replays (match_id, crc, revision, saved_at, sent_by, replay, received_at, attempt_id)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
        params![
            found.id,
            offered.crc,
            offered.revision,
            offered.saved_at,
            offered.sender.as_str(),
            offered.replay,
            ctx.now,
            attempt
        ],
    )?;
    Ok(())
}

/// The confirmed game of set `match_id` that a replay saved at `saved_at`
/// was saved in and that has no replay yet: one whose time, from its permit
/// to its last report (or to the organizer's decision of a game nobody
/// reported), holds `saved_at` give or take `GAME_SLACK`, the one
/// it is nearest, then the first. A replay kept before games were told
/// apart (no `attempt_id`) holds one of the set's confirmed games without a
/// replay, which one unknown, so the set takes a new one only while it has
/// more such games than such replays. A voided game's replay holds none.
fn free_game(tx: &Transaction<'_>, match_id: &str, saved_at: u32) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            "SELECT g.id FROM (
                 SELECT a.id, a.seq, a.created_at AS began, COALESCE(r.ended, d.created_at) AS ended
                 FROM attempts a
                   LEFT JOIN (SELECT attempt_id, MAX(received_at) AS ended FROM game_reports
                              WHERE match_id = ?1 GROUP BY attempt_id) r ON r.attempt_id = a.id
                   LEFT JOIN adjudications d ON d.id = a.adjudication_id
                 WHERE a.match_id = ?1 AND a.state = 'accepted'
                   AND NOT EXISTS (SELECT 1 FROM match_replays m WHERE m.attempt_id = a.id)) g
             WHERE ?2 BETWEEN g.began - ?3 AND g.ended + ?3
               AND (SELECT COUNT(*) FROM match_replays WHERE match_id = ?1 AND attempt_id IS NULL)
                 < (SELECT COUNT(*) FROM attempts f WHERE f.match_id = ?1 AND f.state = 'accepted'
                      AND NOT EXISTS (SELECT 1 FROM match_replays m WHERE m.attempt_id = f.id))
             ORDER BY MAX(g.began - ?2, ?2 - g.ended, 0), g.seq LIMIT 1",
            params![match_id, saved_at, GAME_SLACK],
            |row| row.get(0),
        )
        .optional()?)
}

/// Whether a replay saved at `saved_at` may be one of set `match_id`'s games
/// that is not decided yet: one permitted and awaiting its reports, or held
/// for review, from its permit on, give or take `GAME_SLACK`.
fn unconfirmed(tx: &Transaction<'_>, match_id: &str, saved_at: u32, now: u64) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM attempts WHERE match_id = ?1 AND state IN ('permitted', 'review')
           AND ?2 BETWEEN created_at - ?3 AND ?4 + ?3)",
        params![match_id, saved_at, GAME_SLACK, now],
        |row| row.get(0),
    )?)
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
/// `replay_retention.days` ago and that the archive no longer needs, after
/// writing each set's pack to the backup folder when one is set. A set whose
/// backup cannot be written keeps its replays and is tried again next pass.
/// A pruned set is closed: it takes no more replays (`put`). Runs with the
/// bridge's maintenance.
pub fn prune(tx: &Transaction<'_>, ctx: &Ctx) -> Result<()> {
    let Some(retention) = &ctx.config.replay_retention else {
        return Ok(());
    };
    let cutoff = ctx.now.saturating_sub(retention.days * 24 * 60 * 60);
    // The archive's while it needs them: for a pack not stored yet, whatever
    // the configuration says now, and for games it has not taken in while
    // there is an archive to take them.
    let unarchived = if ctx.config.archive.is_some() {
        sets::NOT_ARCHIVED
    } else {
        "0"
    };
    let old = tx
        .prepare(&format!(
            "SELECT id FROM matches WHERE updated_at <= ?1
               AND EXISTS (SELECT 1 FROM match_replays WHERE match_id = matches.id)
               AND NOT {pack_due} AND NOT {unarchived}
             ORDER BY updated_at LIMIT ?2",
            pack_due = sets::PACK_DUE
        ))?
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
        tx.execute(
            "INSERT INTO archive_sets (match_id, version, next_attempt_at, pruned_at) VALUES (?1, 0, 0, ?2)
             ON CONFLICT (match_id) DO UPDATE SET pruned_at = excluded.pruned_at",
            params![id, ctx.now],
        )?;
    }
    Ok(())
}
