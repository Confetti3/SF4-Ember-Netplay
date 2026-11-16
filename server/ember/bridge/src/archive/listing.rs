//! What `GET /v1/archive` lists (docs/design/REPLAY_ARCHIVE.md, "Listing"):
//! the stored games a filter keeps, newest first or by class, a page at a
//! time, read through the archive's index (`archive_keys`, migration 055) so
//! a page reads about as many games as it lists, however large the archive.
//!
//! A filter is a set of clauses that must all hold: each of its fields is
//! one key (`f:12`, `c:JP`, `k:7`, ...), and a search is any of the names and
//! event titles that hold its text. A page walks the postings of its
//! smallest clause newest first, the driver, and checks the rest on each
//! game's row. A driver with one key is read in order straight off the
//! index; a search's several keys are gathered and sorted, only while they
//! are few (`Limits::gathered`), and otherwise every game is the driver.
//! However the clauses fall, a page looks at no more than `Limits::looked`
//! of its driver's games: a page whose clauses are each common but rare
//! together can hold fewer games, even none, and still go on (`next`).
//!
//! The class order is the newest-first order of the games whose higher class
//! is Grand Master, then Master, down to the games with no class, each a
//! segment the cursor names. A game whose class moves while someone pages
//! through that order moves segment with it, so it can be listed twice or
//! not at all on their pages; the newest order never moves.
use rusqlite::{OptionalExtension, ToSql, Transaction};

use crate::error::Result;

/// The highest class, Grand Master.
pub const TOP_BAND: u8 = 10;

/// What a listing keeps: each field one value, all of them together.
#[derive(Clone, Debug, Default)]
pub struct Filter {
    /// An event's title, or `online` for games of no tournament set.
    pub event: Option<String>,
    pub fighter: Option<u8>,
    pub country: Option<String>,
    /// `top8`, `pools`, `winners`, `losers` or `grand`.
    pub round: Option<String>,
    /// Part of a player's name or an event's title.
    pub q: Option<String>,
    /// A side's name, exactly as the game wrote it (`n:{name}`): a player's
    /// profile lists the sets that carry it. Two players who use one name
    /// share a list, and a player who changed their name has their earlier
    /// games under the earlier one.
    pub name: Option<String>,
    /// A class (band 1 to 10) either side has.
    pub class: Option<u8>,
}

/// The order games are listed in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Order {
    /// Newest first, by save time and then digest.
    Newest,
    /// The higher class of the two sides first, then newest first.
    Class,
}

/// How much one listing does at most.
#[derive(Clone, Copy, Debug)]
pub struct Limits {
    /// Games a page holds.
    pub listed: usize,
    /// Games of its driver a page looks at.
    pub looked: i64,
    /// Games a search's keys may have together to drive a page, gathered and
    /// sorted; past it every game drives.
    pub gathered: i64,
    /// Games of its driver a count looks at; past it the count is unknown.
    pub counted: i64,
}

pub const LIMITS: Limits = Limits {
    listed: ember_protocol::archive::MAX_GAMES,
    looked: 10_000,
    gathered: 20_000,
    counted: 100_000,
};

/// A place in an order: a game's save time and digest, and in the class
/// order its segment, the higher class of its two sides (0 for none). The
/// listing hands it out as `{saved_at}.{digest}` newest first and as
/// `{segment}.{saved_at}.{digest}` by class.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Cursor {
    pub segment: u8,
    pub saved_at: i64,
    pub digest: String,
}

impl Cursor {
    /// Before every game of `order`.
    pub fn first(order: Order) -> Self {
        Self::start(if order == Order::Class { TOP_BAND } else { 0 })
    }

    /// Before every game of segment `segment`: no game is saved at the
    /// latest time there is, so this place reads back like any other.
    fn start(segment: u8) -> Self {
        Self {
            segment,
            saved_at: i64::MAX,
            digest: "f".repeat(64),
        }
    }

    pub fn parse(order: Order, text: &str) -> Option<Self> {
        let (segment, rest) = match order {
            Order::Newest => (0, text),
            Order::Class => {
                let (segment, rest) = text.split_once('.')?;
                let segment: u8 = segment.parse().ok()?;
                (segment <= TOP_BAND).then_some((segment, rest))?
            }
        };
        let (saved_at, digest) = rest.split_once('.')?;
        let hex = digest.len() == 64
            && digest
                .bytes()
                .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'));
        hex.then_some(())?;
        Some(Self {
            segment,
            saved_at: saved_at.parse().ok()?,
            digest: digest.to_owned(),
        })
    }

    pub fn text(&self, order: Order) -> String {
        match order {
            Order::Newest => format!("{}.{}", self.saved_at, self.digest),
            Order::Class => format!("{}.{}.{}", self.segment, self.saved_at, self.digest),
        }
    }
}

/// One listed game.
#[derive(Debug)]
pub struct Listed {
    pub digest: String,
    pub crc: String,
    pub saved_at: i64,
    pub size: u64,
    pub fighters: [u8; 2],
    pub names: [Option<String>; 2],
    pub countries: [Option<String>; 2],
    pub set: Option<String>,
    pub event: Option<String>,
    pub round: Option<String>,
    pub phase: Option<String>,
    pub classes: [Option<u8>; 2],
}

/// One page of a listing: its games and where the next page starts.
pub struct Page {
    pub games: Vec<Listed>,
    pub next: Option<Cursor>,
}

/// One clause of a filter: games with this key, or with any of these.
#[derive(Clone, Debug)]
enum Clause {
    One(String),
    Any(Vec<String>),
}

/// A clause with how many games it has (for `Any`, at most that many).
#[derive(Clone, Debug)]
struct Sized {
    clause: Clause,
    games: i64,
}

/// A filter as its clauses and the values its row check binds.
struct Plan {
    clauses: Vec<Sized>,
    like: Option<String>,
}

/// The search text of `filter` as a LIKE pattern: anywhere in a name or an
/// event's title, its wildcards taken literally.
fn like_of(filter: &Filter) -> Option<String> {
    filter
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
        })
}

fn games_with(tx: &Transaction<'_>, key: &str) -> Result<i64> {
    Ok(tx
        .query_row(
            "SELECT games FROM archive_key_counts WHERE key = ?1",
            [key],
            |row| row.get(0),
        )
        .optional()?
        .unwrap_or(0))
}

/// `filter`'s clauses, with how many games each has; `None` when no game can
/// match (a key no game has, a search nothing holds, a class while classes
/// are hidden).
fn plan(tx: &Transaction<'_>, filter: &Filter, classes_shown: bool) -> Result<Option<Plan>> {
    let mut keys = Vec::new();
    if let Some(event) = &filter.event {
        keys.push(if event == "online" {
            "o".to_owned()
        } else {
            format!("e:{event}")
        });
    }
    if let Some(fighter) = filter.fighter {
        keys.push(format!("f:{fighter}"));
    }
    if let Some(country) = &filter.country {
        keys.push(format!("c:{country}"));
    }
    if let Some(round) = &filter.round {
        keys.push(format!("r:{round}"));
    }
    if let Some(class) = filter.class {
        if !classes_shown {
            return Ok(None);
        }
        keys.push(format!("k:{class}"));
    }
    if let Some(name) = &filter.name {
        keys.push(format!("n:{name}"));
    }
    let mut clauses = Vec::new();
    for key in keys {
        let games = games_with(tx, &key)?;
        if games == 0 {
            return Ok(None);
        }
        clauses.push(Sized {
            clause: Clause::One(key),
            games,
        });
    }
    let like = like_of(filter);
    if let Some(like) = &like {
        // Every name and event title some stored game carries that holds
        // the text, with their games.
        let found = tx
            .prepare(
                "SELECT key, games FROM archive_key_counts
                 WHERE ((key >= 'n:' AND key < 'n;') OR (key >= 'e:' AND key < 'e;'))
                   AND substr(key, 3) LIKE ?1 ESCAPE '\\'",
            )?
            .query_map([like], |row| {
                Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)?))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        if found.is_empty() {
            return Ok(None);
        }
        let games = found.iter().map(|(_, games)| games).sum();
        let mut keys: Vec<String> = found.into_iter().map(|(key, _)| key).collect();
        let clause = if keys.len() == 1 {
            Clause::One(keys.remove(0))
        } else {
            Clause::Any(keys)
        };
        clauses.push(Sized { clause, games });
    }
    Ok(Some(Plan { clauses, like }))
}

/// The check every listed game's row passes: the whole filter, and in the
/// class order the segment it is listed in. It agrees with the keys
/// (`archive_game_keys`), so a game a driver finds is kept just when every
/// clause holds of it.
const KEEPS: &str = "g.state = 'stored'
    AND (:event IS NULL OR (:event = 'online' AND g.match_id IS NULL)
         OR (:event != 'online' AND g.event_title = :event))
    AND (:fighter IS NULL OR g.fighter_a = :fighter OR g.fighter_b = :fighter)
    AND (:country IS NULL OR g.country_a = :country OR g.country_b = :country)
    AND (:round IS NULL
         OR (:round = 'top8' AND g.phase LIKE 'Top%')
         OR (:round = 'pools' AND g.match_id IS NOT NULL AND COALESCE(g.phase, '') NOT LIKE 'Top%')
         OR (:round = 'winners' AND g.round_label LIKE 'Winners%')
         OR (:round = 'losers' AND g.round_label LIKE 'Losers%')
         OR (:round = 'grand' AND g.round_label LIKE 'Grand%'))
    AND (:like IS NULL OR g.name_a LIKE :like ESCAPE '\\' OR g.name_b LIKE :like ESCAPE '\\'
         OR g.event_title LIKE :like ESCAPE '\\')
    AND (:class IS NULL OR g.class_a = :class OR g.class_b = :class)
    AND (:name IS NULL OR g.name_a = :name OR g.name_b = :name)
    AND (:top IS NULL OR MAX(COALESCE(g.class_a, 0), COALESCE(g.class_b, 0)) = :top)";

const COLUMNS: &str = "g.digest, g.crc, g.saved_at, g.size, g.fighter_a, g.fighter_b, g.name_a, g.name_b,
    g.country_a, g.country_b, g.match_id, g.event_title, g.round_label, g.phase, g.class_a, g.class_b";

fn listed_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<Listed> {
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
        classes: [row.get(14)?, row.get(15)?],
    })
}

/// The values `KEEPS` binds for `filter`, with the class order's segment.
fn keeps<'a>(
    filter: &'a Filter,
    like: &'a Option<String>,
    top: &'a Option<u8>,
) -> Vec<(&'static str, &'a dyn ToSql)> {
    vec![
        (":event", &filter.event),
        (":fighter", &filter.fighter),
        (":country", &filter.country),
        (":round", &filter.round),
        (":like", like),
        (":class", &filter.class),
        (":name", &filter.name),
        (":top", top),
    ]
}

/// How a page or a count reads its games.
enum Driver<'a> {
    /// One key's postings, in order.
    One(&'a str),
    /// Several keys' postings, gathered and sorted.
    Any(&'a [String]),
}

/// The clause to walk, of `clauses` (never empty): the one with the fewest
/// games, a search's only while its games are few enough to gather.
fn driver<'a>(clauses: &'a [Sized], gathered: i64) -> (Driver<'a>, i64) {
    clauses
        .iter()
        .filter_map(|sized| match &sized.clause {
            Clause::One(key) => Some((Driver::One(key), sized.games)),
            Clause::Any(keys) if sized.games <= gathered => Some((Driver::Any(keys), sized.games)),
            Clause::Any(_) => None,
        })
        .min_by_key(|(_, games)| *games)
        .unwrap_or((Driver::One("*"), i64::MAX))
}

/// The stored games after `cursor` that `filter` keeps, in `order`: up to
/// `limits.listed` of them, and the cursor that goes on when there may be
/// more.
pub fn page(
    tx: &Transaction<'_>,
    filter: &Filter,
    classes_shown: bool,
    order: Order,
    cursor: &Cursor,
    limits: Limits,
) -> Result<Page> {
    let Some(plan) = plan(tx, filter, classes_shown)? else {
        return Ok(Page {
            games: Vec::new(),
            next: None,
        });
    };
    // Each segment is listed newest first: the class order's from the
    // cursor's down to the games with no class, or the one of the newest. A
    // game with a side of the class a filter picks is in that class's
    // segment or a higher one, so the lower ones are not looked through.
    let lowest = filter.class.unwrap_or(0);
    let segments: Vec<Option<u8>> = match order {
        Order::Newest => vec![None],
        Order::Class => (lowest..=cursor.segment).rev().map(Some).collect(),
    };
    let mut games: Vec<(u8, Listed)> = Vec::new();
    let mut looked_left = limits.looked;
    let mut at = cursor.clone();
    let listed = |games: Vec<(u8, Listed)>| games.into_iter().map(|(_, game)| game).collect();
    for segment in segments {
        let mut clauses = plan.clauses.clone();
        if let Some(segment) = segment {
            let key = format!("t:{segment}");
            let games = games_with(tx, &key)?;
            clauses.push(Sized {
                clause: Clause::One(key),
                games,
            });
        }
        if clauses.is_empty() {
            clauses.push(Sized {
                clause: Clause::One("*".into()),
                games: games_with(tx, "*")?,
            });
        }
        let (driver, size) = driver(&clauses, limits.gathered);
        let segment = segment.unwrap_or(0);
        if size > 0 {
            let wanted = limits.listed + 1 - games.len();
            let top = (order == Order::Class).then_some(segment);
            let Walked {
                games: found,
                floor,
            } = walk(
                tx,
                filter,
                &plan.like,
                top,
                driver,
                &at,
                wanted,
                looked_left,
            )?;
            games.extend(found.into_iter().map(|game| (segment, game)));
            if games.len() > limits.listed {
                games.truncate(limits.listed);
                let (segment, last) = games.last().expect("a full page");
                let next = Cursor {
                    segment: *segment,
                    saved_at: last.saved_at,
                    digest: last.digest.clone(),
                };
                return Ok(Page {
                    games: listed(games),
                    next: Some(next),
                });
            }
            if let Some((saved_at, digest)) = floor {
                // The page looked at all it may: the next goes on from there.
                return Ok(Page {
                    games: listed(games),
                    next: Some(Cursor {
                        segment,
                        saved_at,
                        digest,
                    }),
                });
            }
            looked_left -= size.min(looked_left);
        }
        // This segment is done; the next one, if any, from its start.
        if segment <= lowest {
            break;
        }
        at = Cursor::start(segment - 1);
        if looked_left <= 0 {
            return Ok(Page {
                games: listed(games),
                next: Some(at),
            });
        }
    }
    Ok(Page {
        games: listed(games),
        next: None,
    })
}

/// A page driven by one key: its postings after the cursor and down to the
/// floor, in order straight off `archive_keys`' primary key, each game's row
/// read by its digest. `CROSS JOIN` keeps that order of the two.
fn by_one_key() -> String {
    format!(
        "SELECT {COLUMNS} FROM archive_keys k CROSS JOIN archive_games g ON g.digest = lower(hex(k.digest))
         WHERE k.key = :key AND (k.saved_at, k.digest) < (:before_at, unhex(:before))
           AND (k.saved_at, k.digest) >= (:floor_at, unhex(:floor)) AND {KEEPS}
         ORDER BY k.saved_at DESC, k.digest DESC LIMIT :limit"
    )
}

/// A page driven by several keys: their postings after the cursor gathered
/// once, then sorted.
fn by_any_key() -> String {
    format!(
        "WITH hits AS MATERIALIZED (
             SELECT DISTINCT saved_at, digest FROM archive_keys
             WHERE key IN (SELECT value FROM json_each(:keys))
               AND (saved_at, digest) < (:before_at, unhex(:before)))
         SELECT {COLUMNS} FROM hits h CROSS JOIN archive_games g ON g.digest = lower(hex(h.digest))
         WHERE {KEEPS}
         ORDER BY h.saved_at DESC, h.digest DESC LIMIT :limit"
    )
}

/// What a walk found: its games, and where it stopped looking when its
/// bound stopped it (a game's save time and digest).
struct Walked {
    games: Vec<Listed>,
    floor: Option<(i64, String)>,
}

/// Up to `wanted` games `filter` keeps after `at`, in `segment` (if any), as
/// `driver` finds them, looking at no more than `looked` of the driver's
/// games: the games, and the last game looked at when that bound stopped it.
#[allow(clippy::too_many_arguments)]
fn walk(
    tx: &Transaction<'_>,
    filter: &Filter,
    like: &Option<String>,
    segment: Option<u8>,
    driver: Driver<'_>,
    at: &Cursor,
    wanted: usize,
    looked: i64,
) -> Result<Walked> {
    let limit = wanted as i64;
    let mut values = keeps(filter, like, &segment);
    values.push((":before_at", &at.saved_at));
    values.push((":before", &at.digest));
    values.push((":limit", &limit));
    match driver {
        Driver::One(key) => {
            let floor: Option<(i64, String)> = tx
                .query_row(
                    "SELECT saved_at, lower(hex(digest)) FROM archive_keys
                     WHERE key = ?1 AND (saved_at, digest) < (?2, unhex(?3))
                     ORDER BY saved_at DESC, digest DESC LIMIT 1 OFFSET ?4",
                    rusqlite::params![key, at.saved_at, at.digest, looked.max(1) - 1],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?;
            let (floor_at, floor_digest) = floor.clone().unwrap_or((i64::MIN, String::new()));
            values.push((":key", &key));
            values.push((":floor_at", &floor_at));
            values.push((":floor", &floor_digest));
            let games = tx
                .prepare(&by_one_key())?
                .query_map(values.as_slice(), listed_row)?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            // A page that filled up looked at no further than its last game.
            let floor = floor.filter(|_| games.len() < wanted);
            Ok(Walked { games, floor })
        }
        Driver::Any(keys) => {
            let keys =
                serde_json::to_string(keys).map_err(|_| crate::error::ApiFailure::unavailable())?;
            values.push((":keys", &keys));
            let games = tx
                .prepare(&by_any_key())?
                .query_map(values.as_slice(), listed_row)?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            Ok(Walked { games, floor: None })
        }
    }
}

/// How many stored games there are, and how many of them `filter` keeps,
/// when that is known without looking at more than `limits.counted` games.
pub fn counts(
    tx: &Transaction<'_>,
    filter: &Filter,
    classes_shown: bool,
    limits: Limits,
) -> Result<(i64, Option<i64>)> {
    let total = games_with(tx, "*")?;
    let Some(plan) = plan(tx, filter, classes_shown)? else {
        return Ok((total, Some(0)));
    };
    match plan.clauses.as_slice() {
        [] => return Ok((total, Some(total))),
        [
            Sized {
                clause: Clause::One(_),
                games,
            },
        ] => return Ok((total, Some(*games))),
        _ => {}
    }
    let (driver, size) = driver(&plan.clauses, limits.counted);
    if size > limits.counted {
        return Ok((total, None));
    }
    let top = None;
    let mut values = keeps(filter, &plan.like, &top);
    let matching = match driver {
        Driver::One(key) => {
            values.push((":key", &key));
            tx.query_row(
                &format!(
                    "SELECT COUNT(*) FROM archive_keys k CROSS JOIN archive_games g ON g.digest = lower(hex(k.digest))
                     WHERE k.key = :key AND {KEEPS}"
                ),
                values.as_slice(),
                |row| row.get(0),
            )?
        }
        Driver::Any(keys) => {
            let keys =
                serde_json::to_string(keys).map_err(|_| crate::error::ApiFailure::unavailable())?;
            values.push((":keys", &keys));
            tx.query_row(
                &format!(
                    "SELECT COUNT(*) FROM
                         (SELECT DISTINCT digest FROM archive_keys WHERE key IN (SELECT value FROM json_each(:keys))) h
                     CROSS JOIN archive_games g ON g.digest = lower(hex(h.digest)) WHERE {KEEPS}"
                ),
                values.as_slice(),
                |row| row.get(0),
            )?
        }
    };
    Ok((total, Some(matching)))
}

/// What the listing's events and countries filters can choose from.
pub fn choices(tx: &Transaction<'_>) -> Result<(Vec<String>, Vec<String>)> {
    let events = tx
        .prepare(&format!(
            "SELECT title FROM archive_events ORDER BY last_saved_at DESC LIMIT {}",
            ember_protocol::archive::MAX_EVENTS
        ))?
        .query_map([], |row| row.get::<_, String>(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let countries = tx
        .prepare("SELECT code FROM archive_countries ORDER BY code")?
        .query_map([], |row| row.get::<_, String>(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    Ok((events, countries))
}

#[cfg(test)]
#[path = "listing_tests.rs"]
mod tests;
