//! The public replay archive (docs/design/REPLAY_ARCHIVE.md). Every game a
//! player sends is kept once by its CRC: a tournament set's games through
//! the set's replay route (`replays::put`), any other game through
//! `PUT /v1/replays/{crc}`. `delivery::archive` stores each in the
//! archive's bucket, tournament games first; `GET /v1/archive` lists the
//! stored ones for the archive page, which downloads them from the bucket.
use axum::{
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{EmberId, replays};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Deserialize;
use serde_json::{Value, json};

use crate::{
    AppState,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, json as respond},
    routes::{ledger::scores, play::playing, replays::Described},
};

/// Replays one player may send in a day: far more than anyone plays.
const SENT_PER_DAY: i64 = 3000;
/// What replays waiting for the bucket may take on the bridge together.
const MAX_WAITING: i64 = 2 << 30;
/// Games one listing holds; `before` pages further back.
const LISTED: i64 = 400;

/// What the sending game says of a replay: the two sides' names as it
/// named them, and its own side and country.
#[derive(Deserialize, Default)]
pub struct Sides {
    #[serde(default)]
    a: Option<String>,
    #[serde(default)]
    b: Option<String>,
    #[serde(default)]
    side: Option<u8>,
    #[serde(default)]
    country: Option<String>,
}

fn name(name: &Option<String>) -> Option<String> {
    let name = name.as_deref()?.trim();
    (!name.is_empty() && name.chars().count() <= 64 && !name.chars().any(char::is_control))
        .then(|| name.to_owned())
}

fn country(code: &Option<String>) -> Option<String> {
    code.as_deref()
        .filter(|code| code.len() == 2 && code.bytes().all(|b| b.is_ascii_uppercase()))
        .map(str::to_owned)
}

/// The two fighters (native IDs) an `.emberreplay`'s record names: its menu
/// fields at +47 hold two 34-byte players after 8 bytes, each with its
/// fighter at +16 (`src/common/ReplaySlots.hxx`, ReadRecordInfo).
fn fighters(replay: &[u8]) -> [Option<u8>; 2] {
    [0, 1].map(|side| {
        let fighter = replay[8 + 47 + 8 + side * 34 + 16];
        (fighter < 64).then_some(fighter)
    })
}

/// Keeps the whole replay `replay` (its CRC `crc` checked) for the archive,
/// sent by `sender`. A game it already holds only gains the sender's
/// country and, sent for a set, the set. `set` is the set's ID and what its
/// pack says of it; its games go to the bucket first.
pub fn keep(
    tx: &Transaction<'_>,
    now: u64,
    crc: &str,
    replay: &[u8],
    sender: &EmberId,
    sides: &Sides,
    set: Option<(&str, &Described)>,
) -> Result<()> {
    let own = country(&sides.country);
    let side = sides.side.filter(|side| *side < 2);
    let held = tx
        .query_row("SELECT 1 FROM archive_games WHERE crc = ?1", [crc], |_| {
            Ok(())
        })
        .optional()?
        .is_some();
    let names = [name(&sides.a), name(&sides.b)];
    let (round, phase) = set.map_or((None, None), |(_, described)| {
        let text = |key: &str| {
            described
                .metadata
                .get(key)
                .and_then(Value::as_str)
                .map(str::to_owned)
        };
        (text("round_label"), text("phase"))
    });
    if held {
        if let (Some(country), Some(side)) = (&own, side) {
            let column = if side == 0 { "country_a" } else { "country_b" };
            tx.execute(
                &format!(
                    "UPDATE archive_games SET {column} = COALESCE({column}, ?2) WHERE crc = ?1"
                ),
                params![crc, country],
            )?;
        }
        if let Some((id, described)) = set {
            tx.execute(
                "UPDATE archive_games SET priority = 0, match_id = ?2, event_title = ?3, round_label = ?4, phase = ?5
                 WHERE crc = ?1 AND match_id IS NULL",
                params![crc, id, described.tournament, round, phase],
            )?;
        }
        return Ok(());
    }
    let saved_at = replays::saved_at(crc, replay)
        .ok_or_else(|| ApiFailure::invalid("That is not a whole replay."))?;
    let [fighter_a, fighter_b] = fighters(replay);
    // A set's players by the desk's names when the game sent none.
    let fallback =
        |slot: usize| set.and_then(|(_, described)| described.names.get(slot).cloned().flatten());
    let [name_a, name_b] = [0, 1].map(|slot| names[slot].clone().or_else(|| fallback(slot)));
    let [country_a, country_b] =
        [0, 1].map(|slot| own.clone().filter(|_| side == Some(slot as u8)));
    tx.execute(
        "INSERT INTO archive_games (crc, saved_at, size, fighter_a, fighter_b, name_a, name_b, country_a, country_b,
            match_id, event_title, round_label, phase, sent_by, priority, replay, next_attempt_at, received_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?17)",
        params![
            crc,
            saved_at,
            replay.len(),
            fighter_a,
            fighter_b,
            name_a,
            name_b,
            country_a,
            country_b,
            set.map(|(id, _)| id),
            set.and_then(|(_, described)| described.tournament.clone()),
            round,
            phase,
            sender.as_str(),
            if set.is_some() { 0 } else { 1 },
            replay,
            now
        ],
    )?;
    Ok(())
}

/// `PUT /v1/replays/{crc}?a=&b=&side=&country=`: a player, with their play
/// session, sends a game's `.emberreplay` for the archive. 201 when it is
/// new, 200 when the archive already holds it. Off (404) without an archive.
pub async fn put(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(crc): Path<String>,
    Query(sides): Query<Sides>,
    body: Body<{ replays::MAX_REPLAY }>,
) -> Result<Response> {
    if state.config.archive.is_none() {
        return Err(ApiFailure::not_found());
    }
    let player = playing(&state, &headers).await?;
    let replay = body.0;
    replays::saved_at(&crc, &replay)
        .ok_or_else(|| ApiFailure::invalid("That is not a whole replay with this CRC."))?;
    let ctx = Ctx::of(&state);
    let status = state
        .db
        .write(move |tx| {
            let held = tx
                .query_row("SELECT 1 FROM archive_games WHERE crc = ?1", [&crc], |_| Ok(()))
                .optional()?
                .is_some();
            if !held {
                let (sent, waiting): (i64, i64) = tx.query_row(
                    "SELECT (SELECT COUNT(*) FROM archive_games WHERE sent_by = ?1 AND received_at > ?2),
                            (SELECT COALESCE(SUM(size), 0) FROM archive_games WHERE state = 'pending')",
                    params![player.ember_id.as_str(), ctx.now.saturating_sub(86_400)],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )?;
                if sent >= SENT_PER_DAY {
                    return Err(ApiFailure::rate_limited(3600));
                }
                // Full until the bucket takes what waits; the game sends it again later.
                if waiting + replay.len() as i64 > MAX_WAITING {
                    return Err(ApiFailure::unavailable());
                }
            }
            keep(tx, ctx.now, &crc, &replay, &player.ember_id, &sides, None)?;
            Ok(if held { StatusCode::OK } else { StatusCode::CREATED })
        })
        .await?;
    Ok(respond(status, &json!({})))
}

#[derive(Deserialize)]
pub struct Filter {
    /// An event's title, or `online` for games of no tournament set.
    #[serde(default)]
    event: Option<String>,
    #[serde(default)]
    fighter: Option<u8>,
    #[serde(default)]
    country: Option<String>,
    /// `top8`, `pools`, `winners`, `losers` or `grand`.
    #[serde(default)]
    round: Option<String>,
    /// Part of a player's name or an event's title.
    #[serde(default)]
    q: Option<String>,
    /// Only games saved before this time (unix seconds), to page back.
    #[serde(default)]
    before: Option<u64>,
}

/// `GET /v1/archive`: the stored games the filter keeps, newest first, with
/// no credential, as the archive page shows them: each game's file in the
/// bucket, and each tournament set's score and pack. Also every event and
/// country the archive holds, for the page's filters.
pub async fn list(State(state): State<AppState>, Query(filter): Query<Filter>) -> Result<Response> {
    let Some(archive) = state.config.archive.clone() else {
        return Err(ApiFailure::not_found());
    };
    let base = archive.public_base;
    let listed = state
        .db
        .read(move |tx| {
            let like = filter.q.as_deref().map(str::trim).filter(|q| !q.is_empty() && q.len() <= 64).map(|q| {
                format!("%{}%", q.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_"))
            });
            let mut games = Vec::new();
            let mut sets = serde_json::Map::new();
            let rows = tx
                .prepare(
                    "SELECT crc, saved_at, size, fighter_a, fighter_b, name_a, name_b, country_a, country_b,
                            match_id, event_title, round_label, phase
                     FROM archive_games WHERE state = 'stored'
                       AND (?1 IS NULL OR (?1 = 'online' AND match_id IS NULL) OR event_title = ?1)
                       AND (?2 IS NULL OR fighter_a = ?2 OR fighter_b = ?2)
                       AND (?3 IS NULL OR country_a = ?3 OR country_b = ?3)
                       AND (?4 IS NULL
                            OR (?4 = 'top8' AND phase LIKE 'Top%')
                            OR (?4 = 'pools' AND match_id IS NOT NULL AND COALESCE(phase, '') NOT LIKE 'Top%')
                            OR (?4 = 'winners' AND round_label LIKE 'Winners%')
                            OR (?4 = 'losers' AND round_label LIKE 'Losers%')
                            OR (?4 = 'grand' AND round_label LIKE 'Grand%'))
                       AND (?5 IS NULL OR name_a LIKE ?5 ESCAPE '\\' OR name_b LIKE ?5 ESCAPE '\\'
                            OR event_title LIKE ?5 ESCAPE '\\')
                       AND (?6 IS NULL OR saved_at < ?6)
                     ORDER BY saved_at DESC LIMIT ?7",
                )?
                .query_map(
                    params![filter.event, filter.fighter, filter.country, filter.round, like, filter.before, LISTED + 1],
                    |row| {
                        Ok((
                            row.get::<_, String>(0)?,
                            row.get::<_, u64>(1)?,
                            row.get::<_, u64>(2)?,
                            [row.get::<_, Option<u8>>(3)?, row.get(4)?],
                            [row.get::<_, Option<String>>(5)?, row.get(6)?],
                            [row.get::<_, Option<String>>(7)?, row.get(8)?],
                            row.get::<_, Option<String>>(9)?,
                            row.get::<_, Option<String>>(10)?,
                            row.get::<_, Option<String>>(11)?,
                            row.get::<_, Option<String>>(12)?,
                        ))
                    },
                )?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            let more = rows.len() as i64 > LISTED;
            for (crc, saved_at, size, fighters, names, countries, set, event, round, phase) in
                rows.into_iter().take(LISTED as usize)
            {
                if let Some(id) = &set
                    && !sets.contains_key(id)
                {
                    let score = scores(tx, id).map(|(wins, _)| wins).unwrap_or([0, 0]);
                    sets.insert(id.clone(), json!({ "score": score, "pack": format!("{base}/sets/{id}.emberpack") }));
                }
                games.push(json!({
                    "crc": crc, "url": format!("{base}/games/{crc}.emberreplay"), "saved_at": saved_at, "size": size,
                    "fighters": fighters, "names": names, "countries": countries,
                    "set": set, "event": event, "round": round, "phase": phase,
                }));
            }
            // ponytail: both lists read the whole table; an index per column
            // if the archive grows past a few hundred thousand games.
            let events = tx
                .prepare(
                    "SELECT event_title FROM archive_games WHERE state = 'stored' AND event_title IS NOT NULL
                     GROUP BY event_title ORDER BY MAX(saved_at) DESC LIMIT 100",
                )?
                .query_map([], |row| row.get::<_, String>(0))?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            let countries = tx
                .prepare(
                    "SELECT country_a FROM archive_games WHERE state = 'stored' AND country_a IS NOT NULL
                     UNION SELECT country_b FROM archive_games WHERE state = 'stored' AND country_b IS NOT NULL
                     ORDER BY 1",
                )?
                .query_map([], |row| row.get::<_, String>(0))?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            Ok(json!({ "games": games, "sets": sets, "events": events, "countries": countries, "more": more }))
        })
        .await?;
    Ok(respond(StatusCode::OK, &listed))
}
