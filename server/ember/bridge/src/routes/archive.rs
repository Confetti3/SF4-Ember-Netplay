//! The public replay archive's routes (docs/design/REPLAY_ARCHIVE.md): any
//! game a player sends, `PUT /v1/replays/{crc}`, which `crate::archive`
//! keeps, and `GET /v1/archive`, the stored games for the archive page,
//! which downloads them from the bucket.
use axum::{
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::replays::{self, Replay, Sides};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Deserialize;
use serde_json::{Value, json};

use crate::{
    AppState,
    archive::{self, Admission, Attribution},
    error::{ApiFailure, Result},
    http::{Body, json as respond},
    routes::{ledger::scores, play::playing},
};

/// Games one listing holds at most.
const LISTED: usize = 400;
/// Stored games one listing looks through at most, so a selective filter
/// over a large archive holds the database only briefly. Its `next` goes
/// on from the last game it looked at, so a page can hold fewer games.
const SCANNED: i64 = 5000;

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
    let file = body.0;
    let replay = Replay::parse(&file)
        .filter(|replay| replay.crc_name() == crc)
        .ok_or_else(|| ApiFailure::invalid("That is not a whole replay with this CRC."))?;
    let now = state.now();
    let admitted = state
        .db
        .write(move |tx| {
            archive::admit(
                tx,
                now,
                &file,
                &replay,
                &player.ember_id,
                Attribution::Online(&sides),
            )
        })
        .await?;
    let status = match admitted {
        Admission::New => StatusCode::CREATED,
        Admission::Held => StatusCode::OK,
        Admission::Refused(refused) => return Err(refused.into()),
    };
    Ok(respond(status, &json!({})))
}

#[derive(Deserialize, Default)]
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
    /// A listing's `next`, to go on from where it stopped.
    #[serde(default)]
    cursor: Option<String>,
}

/// A place in the archive's order, newest first: a game's save time and
/// digest. The listing hands it out as `{saved_at}.{digest}`.
#[derive(Clone, Debug, PartialEq)]
struct Cursor {
    saved_at: i64,
    digest: String,
}

impl Cursor {
    /// Before every game.
    fn first() -> Self {
        Self {
            saved_at: i64::MAX,
            digest: String::new(),
        }
    }

    fn parse(text: &str) -> Option<Self> {
        let (saved_at, digest) = text.split_once('.')?;
        (digest.len() == 64
            && digest
                .bytes()
                .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')))
        .then(|| {
            Some(Self {
                saved_at: saved_at.parse().ok()?,
                digest: digest.to_owned(),
            })
        })
        .flatten()
    }

    fn text(&self) -> String {
        format!("{}.{}", self.saved_at, self.digest)
    }
}

/// One page of the listing: its games' rows and where the next page starts.
struct Page {
    games: Vec<Listed>,
    next: Option<Cursor>,
}

struct Listed {
    digest: String,
    crc: String,
    saved_at: i64,
    size: u64,
    fighters: [u8; 2],
    names: [Option<String>; 2],
    countries: [Option<String>; 2],
    set: Option<String>,
    event: Option<String>,
    round: Option<String>,
    phase: Option<String>,
}

/// The stored games after `cursor` that `filter` keeps, newest first: up
/// to `listed` of them among the next `scanned` stored games.
fn page(
    tx: &Transaction<'_>,
    filter: &Filter,
    cursor: &Cursor,
    listed: usize,
    scanned: i64,
) -> Result<Page> {
    // The last game this page may look at, when that many are left.
    let last: Option<Cursor> = tx
        .query_row(
            "SELECT saved_at, digest FROM archive_games
             WHERE state = 'stored' AND (saved_at, digest) < (?1, ?2)
             ORDER BY saved_at DESC, digest DESC LIMIT 1 OFFSET ?3",
            params![cursor.saved_at, cursor.digest, scanned - 1],
            |row| {
                Ok(Cursor {
                    saved_at: row.get(0)?,
                    digest: row.get(1)?,
                })
            },
        )
        .optional()?;
    let floor = last.clone().unwrap_or(Cursor {
        saved_at: i64::MIN,
        digest: String::new(),
    });
    let like = filter
        .q
        .as_deref()
        .map(str::trim)
        .filter(|q| !q.is_empty() && q.len() <= 64)
        .map(|q| {
            format!(
                "%{}%",
                q.replace('\\', "\\\\")
                    .replace('%', "\\%")
                    .replace('_', "\\_")
            )
        });
    let mut games = tx
        .prepare(
            "SELECT digest, crc, saved_at, size, fighter_a, fighter_b, name_a, name_b, country_a, country_b,
                    match_id, event_title, round_label, phase
             FROM archive_games WHERE state = 'stored'
               AND (saved_at, digest) < (?1, ?2) AND (saved_at, digest) >= (?3, ?4)
               AND (?5 IS NULL OR (?5 = 'online' AND match_id IS NULL) OR event_title = ?5)
               AND (?6 IS NULL OR fighter_a = ?6 OR fighter_b = ?6)
               AND (?7 IS NULL OR country_a = ?7 OR country_b = ?7)
               AND (?8 IS NULL
                    OR (?8 = 'top8' AND phase LIKE 'Top%')
                    OR (?8 = 'pools' AND match_id IS NOT NULL AND COALESCE(phase, '') NOT LIKE 'Top%')
                    OR (?8 = 'winners' AND round_label LIKE 'Winners%')
                    OR (?8 = 'losers' AND round_label LIKE 'Losers%')
                    OR (?8 = 'grand' AND round_label LIKE 'Grand%'))
               AND (?9 IS NULL OR name_a LIKE ?9 ESCAPE '\\' OR name_b LIKE ?9 ESCAPE '\\'
                    OR event_title LIKE ?9 ESCAPE '\\')
             ORDER BY saved_at DESC, digest DESC LIMIT ?10",
        )?
        .query_map(
            params![
                cursor.saved_at,
                cursor.digest,
                floor.saved_at,
                floor.digest,
                filter.event,
                filter.fighter,
                filter.country,
                filter.round,
                like,
                listed + 1
            ],
            |row| {
                Ok(Listed {
                    digest: row.get(0)?,
                    crc: row.get(1)?,
                    saved_at: row.get(2)?,
                    size: row.get(3)?,
                    fighters: [row.get(4)?, row.get(5)?],
                    names: [row.get(6)?, row.get(7)?],
                    countries: [row.get(8)?, row.get(9)?],
                    set: row.get(10)?,
                    event: row.get(11)?,
                    round: row.get(12)?,
                    phase: row.get(13)?,
                })
            },
        )?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let next = if games.len() > listed {
        games.truncate(listed);
        games.last().map(|game| Cursor {
            saved_at: game.saved_at,
            digest: game.digest.clone(),
        })
    } else {
        last
    };
    Ok(Page { games, next })
}

/// `GET /v1/archive`: the stored games the filter keeps, newest first, with
/// no credential, as the archive page shows them: each game's file in the
/// bucket, and each tournament set's score and pack once the bucket holds
/// it. `next`, when there is more to look through, is the `cursor` that
/// goes on. Also the events and countries of the archive's games, for the
/// page's filters.
pub async fn list(State(state): State<AppState>, Query(filter): Query<Filter>) -> Result<Response> {
    let Some(archive) = state.config.archive.clone() else {
        return Err(ApiFailure::not_found());
    };
    let cursor = match filter.cursor.as_deref() {
        None => Cursor::first(),
        Some(text) => Cursor::parse(text)
            .ok_or_else(|| ApiFailure::invalid("That is not a cursor of this listing."))?,
    };
    let base = archive.public_base;
    let listed = state
        .db
        .read(move |tx| {
            let Page { games, next } = page(tx, &filter, &cursor, LISTED, SCANNED)?;
            let mut sets = serde_json::Map::new();
            for id in games.iter().filter_map(|game| game.set.as_ref()) {
                if !sets.contains_key(id) {
                    sets.insert(id.clone(), set_listed(tx, &base, id)?);
                }
            }
            let games: Vec<Value> = games
                .into_iter()
                .map(|game| {
                    json!({
                        "digest": game.digest, "crc": game.crc,
                        "url": format!("{base}/games/{}.emberreplay", game.digest),
                        "saved_at": game.saved_at, "size": game.size, "fighters": game.fighters,
                        "names": game.names, "countries": game.countries,
                        "set": game.set, "event": game.event, "round": game.round, "phase": game.phase,
                    })
                })
                .collect();
            let events = tx
                .prepare("SELECT title FROM archive_events ORDER BY last_saved_at DESC LIMIT 100")?
                .query_map([], |row| row.get::<_, String>(0))?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            let countries = tx
                .prepare("SELECT code FROM archive_countries ORDER BY code")?
                .query_map([], |row| row.get::<_, String>(0))?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            Ok(json!({
                "games": games, "sets": sets, "events": events, "countries": countries,
                "more": next.is_some(), "next": next.map(|next| next.text()),
            }))
        })
        .await?;
    Ok(respond(StatusCode::OK, &listed))
}

/// What the listing says of set `id`: its score, and its pack's address
/// once the bucket holds one.
fn set_listed(tx: &Transaction<'_>, base: &str, id: &str) -> Result<Value> {
    let score = scores(tx, id).map(|(wins, _)| wins).unwrap_or([0, 0]);
    let packed: bool = tx
        .query_row(
            "SELECT uploaded_version > 0 FROM archive_sets WHERE match_id = ?1",
            [id],
            |row| row.get(0),
        )
        .optional()?
        .unwrap_or(false);
    let pack = packed.then(|| format!("{base}/sets/{id}.emberpack"));
    Ok(json!({ "score": score, "pack": pack }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Db;

    /// Stored games `(saved_at, digest letter, fighter)`.
    async fn archive_of(games: &'static [(i64, char, u8)]) -> Db {
        let db = Db::open(std::path::Path::new(":memory:")).unwrap();
        db.write(move |tx| {
            for (saved_at, letter, fighter) in games {
                tx.execute(
                    "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, sent_by, priority,
                        state, next_attempt_at, received_at)
                     VALUES (?1, '00000000', ?2, 1, ?3, ?3, 'emb_p1', 1, 'stored', 0, 0)",
                    params![letter.to_string().repeat(64), saved_at, fighter],
                )?;
            }
            Ok(())
        })
        .await
        .unwrap();
        db
    }

    /// Every game `filter` keeps, page by page, and the pages' sizes.
    async fn every_page(
        db: &Db,
        filter: Filter,
        listed: usize,
        scanned: i64,
    ) -> (Vec<char>, Vec<usize>) {
        let filter = std::sync::Arc::new(filter);
        let (mut seen, mut sizes, mut cursor) = (Vec::new(), Vec::new(), Some(Cursor::first()));
        while let Some(at) = cursor {
            let filter = filter.clone();
            let Page { games, next } = db
                .read(move |tx| page(tx, &filter, &at, listed, scanned))
                .await
                .unwrap();
            sizes.push(games.len());
            seen.extend(games.iter().map(|game| game.digest.chars().next().unwrap()));
            // Each page's cursor reads back as itself.
            cursor = next.map(|next| Cursor::parse(&next.text()).unwrap());
        }
        (seen, sizes)
    }

    /// Games saved in the same second, across a page's end, are each listed
    /// once, in one order.
    #[tokio::test]
    async fn pages_part_at_a_game_not_at_a_second() {
        let db = archive_of(&[
            (5, 'a', 1),
            (5, 'b', 1),
            (5, 'c', 1),
            (5, 'd', 1),
            (4, 'e', 1),
            (6, 'f', 1),
        ])
        .await;
        let (seen, sizes) = every_page(&db, Filter::default(), 2, 100).await;
        assert_eq!(seen, ['f', 'd', 'c', 'b', 'a', 'e']);
        assert_eq!(sizes, [2, 2, 2]);
    }

    /// A filter that keeps few games looks through a bounded stretch per
    /// page, and goes on from where it stopped.
    #[tokio::test]
    async fn a_selective_filter_looks_through_a_bounded_stretch() {
        let db = archive_of(&[
            (1, 'a', 9),
            (2, 'b', 1),
            (3, 'c', 1),
            (4, 'd', 1),
            (5, 'e', 9),
            (6, 'f', 1),
        ])
        .await;
        let filter = Filter {
            fighter: Some(9),
            ..Filter::default()
        };
        let (seen, sizes) = every_page(&db, filter, 10, 2).await;
        assert_eq!(seen, ['e', 'a']);
        assert_eq!(sizes, [1, 0, 1, 0]);
    }

    #[test]
    fn only_a_listings_cursor_reads() {
        let digest = "0".repeat(64);
        assert_eq!(
            Cursor::parse(&format!("12.{digest}")),
            Some(Cursor {
                saved_at: 12,
                digest: digest.clone()
            })
        );
        for text in [
            "12",
            "x.{digest}",
            &format!("12.{}", &digest[1..]),
            &format!("12.{}", digest.to_uppercase().replace('0', "A")),
        ] {
            assert_eq!(Cursor::parse(text), None, "{text}");
        }
    }
}
