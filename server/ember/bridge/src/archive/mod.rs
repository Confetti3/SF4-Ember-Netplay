//! The public replay archive (docs/design/REPLAY_ARCHIVE.md). A game enters
//! it only through `admit`, once, known by its `replays::Replay::game` and
//! kept under the digest of the copy that came first: any game a player sends through
//! `PUT /v1/replays/{crc}` (`routes::archive`), and a tournament set's games
//! as the set holds them (`sets`). `delivery::archive` stores each game in
//! the bucket, tournament games first, and `delivery::packs` each set's
//! pack; `GET /v1/archive` lists what is stored. Games kept before they were
//! known by `game` are folded into one by `duplicates`.
pub mod classes;
pub mod duplicates;
pub mod listing;
pub mod sets;

use ember_protocol::{
    EmberId,
    replays::{Replay, Sides},
    rooms::{is_country, is_player_name},
};
use rusqlite::{OptionalExtension, Transaction, params};

use crate::error::{ApiFailure, Result};

/// Games one player may send in a day: far more than anyone plays.
const SENT_PER_DAY: i64 = 3000;
/// What games waiting for the bucket may take on the bridge together.
const MAX_WAITING: i64 = 2 << 30;
/// What the archive keeps at most, every game it holds together: about two
/// million games. Past it no online game is taken; a set's games still are.
const MAX_KEPT: i64 = 20 << 30;

/// Whose word the archive takes for a game's sides.
pub enum Attribution<'a> {
    /// A game outside any set: its sender's side, and that side's name and
    /// country. The copy that comes first says what the game was; a later
    /// copy from another account names its own side if it has no name yet
    /// (`name_own_side`).
    Online(&'a Sides),
    /// A game of a tournament set, sided by the set's roster.
    Set(&'a sets::SetGame),
}

/// What became of a game offered to the archive.
pub enum Admission {
    New,
    /// The archive already holds this game.
    Held,
    Refused(Refused),
}

/// Why the archive takes no new game for now. The sender's game sends it
/// again later.
pub enum Refused {
    /// The sender sent `SENT_PER_DAY` games in the last day.
    SendersDay,
    /// `MAX_WAITING` of games wait for the bucket.
    Full,
    /// The archive keeps `MAX_KEPT` of games already.
    Kept,
}

impl From<Refused> for ApiFailure {
    fn from(refused: Refused) -> Self {
        match refused {
            Refused::SendersDay => ApiFailure::rate_limited(3600),
            Refused::Full | Refused::Kept => ApiFailure::unavailable(),
        }
    }
}

/// Offers `file`, read as `replay`, sent by `sender`, to the archive in the
/// caller's transaction. A game the archive holds keeps what its first copy
/// said; only a set's own player can add to a set's game (`SetGame::held`),
/// and only an account other than the first sender can name an empty side of
/// an online game (`name_own_side`). A new
/// game is taken when there is room for it (`refusal`), with what
/// `attribution` says of its sides. A set's copy of a game is kept over its
/// own players' online copies of it, which are folded into it
/// (`duplicates::fold`); only the same file is the same online game to a set.
/// A set's copy of a file folded away before takes that file's row back.
pub fn admit(
    tx: &Transaction<'_>,
    now: u64,
    file: &[u8],
    replay: &Replay,
    sender: &EmberId,
    attribution: Attribution<'_>,
) -> Result<Admission> {
    let (digest, game) = (replay.digest_hex(), replay.game_hex());
    let for_set = matches!(attribution, Attribution::Set(_));
    let held: Option<(String, String, Option<String>)> = tx
        .query_row(
            "SELECT digest, sent_by, match_id FROM archive_games
             WHERE (digest = ?1 OR (game = ?2 AND (NOT ?3 OR match_id IS NOT NULL))) AND state != 'duplicate'
             ORDER BY digest = ?1 DESC, match_id IS NULL, received_at, digest LIMIT 1",
            params![digest, game, for_set],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
        )
        .optional()?;
    if let Some((held, sent_by, set)) = held {
        match attribution {
            Attribution::Set(game) => {
                game.held(tx, now, &held, &sent_by, set.as_deref(), sender)?
            }
            Attribution::Online(sides) => name_own_side(tx, &held, sender, sides)?,
        }
        return Ok(Admission::Held);
    }
    let (names, countries, set, sent_side) = match attribution {
        Attribution::Online(sides) => {
            let (names, countries) = online_sides(sides);
            (names, countries, None, sides.side.filter(|side| *side < 2))
        }
        Attribution::Set(game) => (game.names.clone(), game.countries.clone(), Some(game), None),
    };
    if let Some(refused) = refusal(tx, now, sender, file.len(), set.is_none())? {
        return Ok(Admission::Refused(refused));
    }
    tx.execute(
        "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, name_a, name_b, country_a, country_b,
            sent_side, match_id, event_title, round_label, phase, sent_by, charged, priority, replay, next_attempt_at, received_at, game)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?20, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?19, ?21)
         ON CONFLICT (digest) DO UPDATE SET
            name_a = excluded.name_a, name_b = excluded.name_b, country_a = excluded.country_a,
            country_b = excluded.country_b, sent_side = excluded.sent_side, match_id = excluded.match_id,
            event_title = excluded.event_title, round_label = excluded.round_label, phase = excluded.phase,
            sent_by = excluded.sent_by, charged = excluded.charged, priority = excluded.priority,
            replay = excluded.replay, received_at = excluded.received_at, named_by_a = NULL, named_by_b = NULL,
            state = 'pending', stored_at = NULL, last_error = NULL, first_attempt_at = NULL,
            next_attempt_at = MAX(archive_games.next_attempt_at, excluded.next_attempt_at)
         WHERE archive_games.state = 'duplicate'",
        params![
            digest,
            replay.crc_name(),
            replay.saved_at,
            file.len(),
            replay.fighters[0],
            replay.fighters[1],
            names[0],
            names[1],
            countries[0],
            countries[1],
            set.map(|game| &game.match_id),
            set.and_then(|game| game.event.as_ref()),
            set.and_then(|game| game.round.as_ref()),
            set.and_then(|game| game.phase.as_ref()),
            sender.as_str(),
            set.is_none(),
            if set.is_some() { 0 } else { 1 },
            file,
            now,
            sent_side,
            game
        ],
    )?;
    crate::db::queued();
    if let Some(set) = set {
        crate::delivery::packs::changed(tx, &set.match_id, now)?;
        duplicates::fold(tx, &game)?;
    }
    Ok(Admission::New)
}

/// Why a new game of `size` bytes from `sender` cannot be taken now, if it
/// cannot. The sender's daily count is of the games it paid for (`charged`,
/// online games), and a set's games skip it (`daily` false): the games the
/// set played bound them already, and so is the bound on what the archive
/// keeps. Nothing skips the bound on what waits for the bucket.
fn refusal(
    tx: &Transaction<'_>,
    now: u64,
    sender: &EmberId,
    size: usize,
    daily: bool,
) -> Result<Option<Refused>> {
    let (sent, waiting, kept): (i64, i64, i64) = tx.query_row(
        "SELECT (SELECT COUNT(*) FROM archive_games WHERE sent_by = ?1 AND charged = 1 AND received_at > ?2),
                (SELECT COALESCE(SUM(size), 0) FROM archive_games WHERE state = 'pending'),
                (SELECT bytes FROM archive_kept)",
        params![sender.as_str(), now.saturating_sub(86_400)],
        |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
    )?;
    Ok(if daily && sent >= SENT_PER_DAY {
        Some(Refused::SendersDay)
    } else if daily && kept + size as i64 > MAX_KEPT {
        Some(Refused::Kept)
    } else if waiting + size as i64 > MAX_WAITING {
        Some(Refused::Full)
    } else {
        None
    })
}

/// The names and countries of an online game's two sides as its sender's
/// game gave them: its own name and country on its own side, and nothing on
/// the other. A player's name is theirs to share: an opponent who shares
/// nothing is never named by the copy of someone who does, so what a sender
/// says of the other side (older games sent both names) is ignored, and a
/// copy that does not say which side is the sender's names no one.
fn online_sides(sides: &Sides) -> ([Option<String>; 2], [Option<String>; 2]) {
    let country = sides.country.clone().filter(|code| is_country(code));
    let countries = [0, 1].map(|side| country.clone().filter(|_| sides.side == Some(side)));
    ([0, 1].map(|side| own_name(sides, side)), countries)
}

/// The name `sides` gives side `side`, when that side is its sender's own.
fn own_name(sides: &Sides, side: u8) -> Option<String> {
    if sides.side != Some(side) {
        return None;
    }
    let name = if side == 0 { &sides.a } else { &sides.b };
    name.as_deref()
        .map(str::trim)
        .filter(|name| is_player_name(name))
        .map(str::to_owned)
}

/// What a later copy of the online game `digest`, from `sender`, adds to
/// it: its own side's name and country (`name_side`).
fn name_own_side(
    tx: &Transaction<'_>,
    digest: &str,
    sender: &EmberId,
    sides: &Sides,
) -> Result<()> {
    let Some(side) = sides.side.filter(|side| *side < 2) else {
        return Ok(());
    };
    let (names, countries) = online_sides(sides);
    let at = usize::from(side);
    let (name, country) = (names[at].as_deref(), countries[at].as_deref());
    name_side(tx, digest, side, name, country, sender.as_str())
}

/// Names side `side` of the online game `digest` the archive already holds
/// `name`, with its `country`, as `by`'s copy names it, when that side has
/// no name yet. The first sender's copy is the one that said what the game
/// was, so `by` adds nothing when it is the game's first sender, nor for
/// the side the first sender said was theirs; one account names at most one
/// side. Who gave each name is kept (`named_by_a`, `named_by_b`). A name
/// once given stays. The archive cannot check that `by` played the side it
/// names, only that it is not the game's first sender.
pub(crate) fn name_side(
    tx: &Transaction<'_>,
    digest: &str,
    side: u8,
    name: Option<&str>,
    country: Option<&str>,
    by: &str,
) -> Result<()> {
    let Some(name) = name else {
        return Ok(());
    };
    let (column, country_column, named_by, other_by) = match side {
        0 => ("name_a", "country_a", "named_by_a", "named_by_b"),
        _ => ("name_b", "country_b", "named_by_b", "named_by_a"),
    };
    let named = tx.execute(
        &format!(
            "UPDATE archive_games SET {column} = ?2, {named_by} = ?3, {country_column} = COALESCE({country_column}, ?4)
             WHERE digest = ?1 AND match_id IS NULL AND {column} IS NULL AND sent_by != ?3
               AND COALESCE(sent_side, -1) != {side} AND COALESCE({other_by}, '') != ?3"
        ),
        params![digest, name, by, country],
    )?;
    if named > 0 && country.is_some() {
        add_choices(tx, digest)?;
    }
    Ok(())
}

/// Adds stored game `digest`'s event and countries to the listing's filter
/// choices. Run when a game is stored, and when a stored game joins a set
/// or gains a country.
pub fn add_choices(tx: &Transaction<'_>, digest: &str) -> Result<()> {
    tx.execute(
        "INSERT INTO archive_events (title, last_saved_at)
         SELECT event_title, saved_at FROM archive_games
         WHERE digest = ?1 AND state = 'stored' AND event_title IS NOT NULL
         ON CONFLICT (title) DO UPDATE SET last_saved_at = MAX(last_saved_at, excluded.last_saved_at)",
        [digest],
    )?;
    tx.execute(
        "INSERT OR IGNORE INTO archive_countries (code)
         SELECT country_a FROM archive_games WHERE digest = ?1 AND state = 'stored' AND country_a IS NOT NULL
         UNION SELECT country_b FROM archive_games WHERE digest = ?1 AND state = 'stored' AND country_b IS NOT NULL",
        [digest],
    )?;
    Ok(())
}
