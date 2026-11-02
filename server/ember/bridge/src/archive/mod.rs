//! The public replay archive (docs/design/REPLAY_ARCHIVE.md). A game enters
//! it only through `admit`, once, under its replay's digest
//! (`replays::Replay`): any game a player sends through
//! `PUT /v1/replays/{crc}` (`routes::archive`), and a tournament set's games
//! as the set holds them (`sets`). `delivery::archive` stores each game in
//! the bucket, tournament games first, and `delivery::packs` each set's
//! pack; `GET /v1/archive` lists what is stored.
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

/// Whose word the archive takes for a game's sides.
pub enum Attribution<'a> {
    /// A game outside any set: its sender's side, and that side's name and
    /// country. The copy that comes first says what the game was; a later
    /// copy only names its own player's side if it has no name yet.
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
}

impl From<Refused> for ApiFailure {
    fn from(refused: Refused) -> Self {
        match refused {
            Refused::SendersDay => ApiFailure::rate_limited(3600),
            Refused::Full => ApiFailure::unavailable(),
        }
    }
}

/// Offers `file`, read as `replay`, sent by `sender`, to the archive in the
/// caller's transaction. A game the archive holds keeps what its first copy
/// said; only a set's own player can add to it (`SetGame::held`). A new
/// game is taken when there is room for it (`refusal`), with what
/// `attribution` says of its sides.
pub fn admit(
    tx: &Transaction<'_>,
    now: u64,
    file: &[u8],
    replay: &Replay,
    sender: &EmberId,
    attribution: Attribution<'_>,
) -> Result<Admission> {
    let digest = replay.digest_hex();
    let held: Option<(String, Option<String>)> = tx
        .query_row(
            "SELECT sent_by, match_id FROM archive_games WHERE digest = ?1",
            [&digest],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?;
    if let Some((sent_by, set)) = held {
        match attribution {
            Attribution::Set(game) => {
                game.held(tx, now, &digest, &sent_by, set.as_deref(), sender)?
            }
            Attribution::Online(sides) => name_own_side(tx, &digest, sides)?,
        }
        return Ok(Admission::Held);
    }
    let (names, countries, set) = match attribution {
        Attribution::Online(sides) => {
            let (names, countries) = online_sides(sides);
            (names, countries, None)
        }
        Attribution::Set(game) => (game.names.clone(), game.countries.clone(), Some(game)),
    };
    if let Some(refused) = refusal(tx, now, sender, file.len(), set.is_none())? {
        return Ok(Admission::Refused(refused));
    }
    tx.execute(
        "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, name_a, name_b, country_a, country_b,
            match_id, event_title, round_label, phase, sent_by, charged, priority, replay, next_attempt_at, received_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?19)",
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
            now
        ],
    )?;
    crate::db::queued();
    if let Some(game) = set {
        crate::delivery::packs::changed(tx, &game.match_id, now)?;
    }
    Ok(Admission::New)
}

/// Why a new game of `size` bytes from `sender` cannot be taken now, if it
/// cannot. The sender's daily count is of the games it paid for (`charged`,
/// online games), and a set's games skip it (`daily` false): the games the
/// set played bound them already. Nothing skips the bound on what waits for
/// the bucket.
fn refusal(
    tx: &Transaction<'_>,
    now: u64,
    sender: &EmberId,
    size: usize,
    daily: bool,
) -> Result<Option<Refused>> {
    let (sent, waiting): (i64, i64) = tx.query_row(
        "SELECT (SELECT COUNT(*) FROM archive_games WHERE sent_by = ?1 AND charged = 1 AND received_at > ?2),
                (SELECT COALESCE(SUM(size), 0) FROM archive_games WHERE state = 'pending')",
        params![sender.as_str(), now.saturating_sub(86_400)],
        |row| Ok((row.get(0)?, row.get(1)?)),
    )?;
    Ok(if daily && sent >= SENT_PER_DAY {
        Some(Refused::SendersDay)
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

/// Adds to the online game `digest` the archive already holds the name its
/// own player's copy gives that player's side, when the side has none yet.
/// Each side is named only by its own player's copy; a name once given stays.
fn name_own_side(tx: &Transaction<'_>, digest: &str, sides: &Sides) -> Result<()> {
    for (side, column) in [(0, "name_a"), (1, "name_b")] {
        if let Some(name) = own_name(sides, side) {
            tx.execute(
                &format!("UPDATE archive_games SET {column} = ?2 WHERE digest = ?1 AND match_id IS NULL AND {column} IS NULL"),
                params![digest, name],
            )?;
        }
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
