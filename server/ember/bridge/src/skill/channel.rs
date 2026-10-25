//! The replay channel's records (docs/design/REPLAY_CHANNEL.md): the games
//! public room hosts reported, the replays players uploaded, and each
//! Discord account's choice to share its games.
//!
//! A replay is listed once the bridge knows both players' accounts. For a
//! public room that is the host's report of the game: an upload of a public
//! room's game before its report is refused as not reported yet, and the
//! game sends it again. For a private room it is both players' uploads of
//! the same game. A side's name and country are what
//! that side's own upload sent; a side that never uploaded shows the
//! listing it chose for the ranking board. Whether an account shares is
//! read each time a page is made, so changing it takes effect at once.
use std::collections::HashSet;

use ember_protocol::{
    EmberId,
    api::ErrorCode,
    replays::{
        ChannelReplay, ChannelSide, ExportInfo, ListQuery, NOT_REPORTED, NOT_YOUR_GAME, PAGE_SIZE,
        ReplayPage, ReplaySharing, ReplayUpload, RoomKind, UploadAnswer, UploadState,
    },
    skill::{NO_ACCOUNT, SkillBadge},
};
use rusqlite::{OptionalExtension, Row as SqlRow, Transaction, params, types::Value};

use super::{Developers, account, badge_of};
use crate::{
    error::{ApiFailure, Result},
    util::{hex, random},
};

/// How long a reported game is kept, since an upload may follow the report.
const GAME_SECS: u64 = 14 * 24 * 60 * 60;
/// How long an upload waits for the other player's before it is dropped.
const HELD_SECS: u64 = 2 * 24 * 60 * 60;
/// The most listed replays, and the most bytes of replay files, kept.
pub const MAX_ROWS: i64 = 200_000;
pub const MAX_BYTES: i64 = 10 * 1024 * 1024 * 1024;

/// How classes are shown (`rooms.skill_badges` and `rooms.developers`).
#[derive(Clone, Copy)]
pub struct Show<'a> {
    pub classes_shown: bool,
    pub developers: &'a [String],
}

/// One side of a stored game.
#[derive(Clone, Default)]
struct Side {
    account: Option<String>,
    name: Option<String>,
    country: Option<String>,
    uploaded: bool,
    badge: Option<SkillBadge>,
}

/// A stored game.
struct Stored {
    seq: i64,
    id: String,
    kind: RoomKind,
    sides: [Side; 2],
    fighters: [u8; 2],
    winner: Option<u8>,
    published: bool,
}

const COLUMNS: &str = "seq, id, room_kind, account0, name0, country0, uploaded0, badge0,
    account1, name1, country1, uploaded1, badge1, fighter0, fighter1, winner, published";

fn kind_text(kind: RoomKind) -> &'static str {
    match kind {
        RoomKind::Public => "public",
        RoomKind::Private => "private",
    }
}

fn kind_of(text: &str) -> RoomKind {
    if text == "public" {
        RoomKind::Public
    } else {
        RoomKind::Private
    }
}

fn badge_of_text(text: Option<String>) -> Option<SkillBadge> {
    text.and_then(|text| serde_json::from_str(&text).ok())
}

fn stored(row: &SqlRow<'_>) -> rusqlite::Result<Stored> {
    let side = |first: usize| -> rusqlite::Result<Side> {
        Ok(Side {
            account: row.get(first)?,
            name: row.get(first + 1)?,
            country: row.get(first + 2)?,
            uploaded: row.get(first + 3)?,
            badge: badge_of_text(row.get(first + 4)?),
        })
    };
    Ok(Stored {
        seq: row.get(0)?,
        id: row.get(1)?,
        kind: kind_of(&row.get::<_, String>(2)?),
        sides: [side(3)?, side(8)?],
        fighters: [row.get(13)?, row.get(14)?],
        winner: row.get(15)?,
        published: row.get(16)?,
    })
}

fn find(tx: &Transaction<'_>, room_id: &str, table: u8, generation: u64) -> Result<Option<Stored>> {
    Ok(tx
        .query_row(
            &format!(
                "SELECT {COLUMNS} FROM channel_replays
                 WHERE room_id = ?1 AND table_id = ?2 AND generation = ?3"
            ),
            params![room_id, table, generation],
            stored,
        )
        .optional()?)
}

fn badge_json(side: &Side) -> Option<String> {
    side.badge
        .as_ref()
        .and_then(|badge| serde_json::to_string(badge).ok())
}

fn band_of(side: &Side) -> Option<u8> {
    side.badge
        .filter(|badge| !badge.developer && badge.band >= 1)
        .map(|badge| badge.band)
}

/// Writes everything about the game that can change after it is first stored.
fn save(tx: &Transaction<'_>, game: &Stored) -> Result<()> {
    let [one, other] = &game.sides;
    tx.execute(
        "UPDATE channel_replays SET room_kind = ?2, winner = ?3, published = ?4,
             account0 = ?5, name0 = ?6, country0 = ?7, uploaded0 = ?8, badge0 = ?9, band0 = ?10,
             account1 = ?11, name1 = ?12, country1 = ?13, uploaded1 = ?14, badge1 = ?15, band1 = ?16
         WHERE seq = ?1",
        params![
            game.seq,
            kind_text(game.kind),
            game.winner,
            game.published,
            one.account,
            one.name,
            one.country,
            one.uploaded,
            badge_json(one),
            band_of(one),
            other.account,
            other.name,
            other.country,
            other.uploaded,
            badge_json(other),
            band_of(other),
        ],
    )?;
    Ok(())
}

/// What an upload brings to a game.
pub struct Upload<'a> {
    pub account: &'a str,
    pub request: &'a ReplayUpload,
    pub info: ExportInfo,
    pub sha256: &'a str,
    pub size: u32,
}

fn refuse(reason: &'static str, message: &'static str) -> ApiFailure {
    ApiFailure::new(ErrorCode::Forbidden, message).detail("reason", reason)
}

fn not_your_game() -> ApiFailure {
    refuse(NOT_YOUR_GAME, "That game is not one this account played.")
}

fn not_reported() -> ApiFailure {
    ApiFailure::new(
        ErrorCode::ServiceUnavailable,
        "The room has not reported that game yet; send it again later.",
    )
    .detail("reason", NOT_REPORTED)
}

/// Whether `room_id` is a public room's: open now, or one with a reported game.
fn public_room(tx: &Transaction<'_>, room_id: &str) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM rooms WHERE room_id = ?1)
             OR EXISTS (SELECT 1 FROM channel_games WHERE room_id = ?1)",
        [room_id],
        |row| row.get(0),
    )?)
}

/// Whether `account` shares its games.
fn sharing_on(tx: &Transaction<'_>, account: &str) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT COALESCE((SELECT sharing FROM replay_sharing WHERE discord_user_id = ?1), 1)",
        [account],
        |row| row.get(0),
    )?)
}

/// The class `account` shows now, as the ranking would show it: none while
/// classes are hidden, for a player who does not take part, or before a first
/// class.
fn badge(
    tx: &Transaction<'_>,
    account: &str,
    show: Show<'_>,
    now: u64,
) -> Result<Option<SkillBadge>> {
    let ranked: bool = tx.query_row(
        "SELECT COALESCE((SELECT ranked FROM ranking_choices WHERE discord_user_id = ?1), 1)",
        [account],
        |row| row.get(0),
    )?;
    if !ranked || !show.classes_shown {
        return Ok(None);
    }
    let developers = Developers {
        accounts: show.developers,
        shown: true,
    };
    if developers.mark(account) {
        return Ok(Some(SkillBadge::DEVELOPER));
    }
    let badge = badge_of(tx, account, now)?;
    Ok((badge.band != 0).then_some(badge))
}

/// How the ranking board lists `account`, if it does.
fn listing(tx: &Transaction<'_>, account: &str) -> Result<Option<(String, Option<String>)>> {
    Ok(tx
        .query_row(
            "SELECT name, country FROM ranking_choices
             WHERE discord_user_id = ?1 AND ranked AND name IS NOT NULL",
            [account],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?)
}

/// Lists the game: gives a side that never uploaded its board listing (or a
/// default name) and both sides the class they show now.
fn publish(tx: &Transaction<'_>, game: &mut Stored, show: Show<'_>, now: u64) -> Result<()> {
    for (index, side) in game.sides.iter_mut().enumerate() {
        let Some(account) = side.account.clone() else {
            continue;
        };
        if side.name.is_none() {
            let (name, country) =
                listing(tx, &account)?.unwrap_or_else(|| (format!("Player {}", index + 1), None));
            side.name = Some(name);
            side.country = country;
        }
        side.badge = badge(tx, &account, show, now)?;
    }
    game.published = true;
    Ok(())
}

/// A game's first row, for the side that uploaded it.
fn insert(
    tx: &Transaction<'_>,
    upload: &Upload<'_>,
    sides: [Side; 2],
    kind: RoomKind,
    winner: Option<u8>,
    now: u64,
) -> Result<Stored> {
    let request = upload.request;
    let id = hex(&random::<16>());
    // A game cannot have been played after it is uploaded.
    let played_at = upload.info.played_at.min(now);
    tx.execute(
        "INSERT INTO channel_replays
             (id, room_id, table_id, generation, room_kind, uploaded0, uploaded1,
              fighter0, fighter1, played_at, sha256, size, published, created_at)
         VALUES (?1, ?2, ?3, ?4, ?5, 0, 0, ?6, ?7, ?8, ?9, ?10, 0, ?11)",
        params![
            id,
            request.room_id,
            request.table,
            request.generation,
            kind_text(kind),
            upload.info.fighters[0],
            upload.info.fighters[1],
            played_at,
            upload.sha256,
            upload.size,
            now,
        ],
    )?;
    Ok(Stored {
        seq: tx.last_insert_rowid(),
        id,
        kind,
        sides,
        fighters: upload.info.fighters,
        winner,
        published: false,
    })
}

/// The side an upload speaks for, from the upload's own name and country.
fn own_side(upload: &Upload<'_>) -> Side {
    Side {
        account: Some(upload.account.to_owned()),
        name: Some(upload.request.name.clone()),
        country: upload.request.country.clone(),
        uploaded: true,
        badge: None,
    }
}

/// Stores `upload` and says what became of it. `account` is the uploader's
/// Discord account.
pub fn upload(
    tx: &Transaction<'_>,
    show: Show<'_>,
    upload: &Upload<'_>,
    now: u64,
) -> Result<UploadAnswer> {
    let request = upload.request;
    let (side, other) = (usize::from(request.side), 1 - usize::from(request.side));
    let account = upload.account;
    let found = find(tx, &request.room_id, request.table, request.generation)?;
    let game = match found {
        None => {
            let reported: Option<(String, String)> = tx
                .query_row(
                    "SELECT winner, loser FROM channel_games
                     WHERE room_id = ?1 AND table_id = ?2 AND generation = ?3",
                    params![request.room_id, request.table, request.generation],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?;
            let mut sides = [Side::default(), Side::default()];
            sides[side] = own_side(upload);
            match reported {
                Some((winner, loser)) => {
                    let opponent = if winner == account {
                        loser
                    } else if loser == account {
                        winner.clone()
                    } else {
                        return Err(not_your_game());
                    };
                    sides[other].account = Some(opponent);
                    let won = if winner == account { side } else { other };
                    let mut game =
                        insert(tx, upload, sides, RoomKind::Public, Some(won as u8), now)?;
                    publish(tx, &mut game, show, now)?;
                    game
                }
                // A public room's game waits for its report: nobody but its
                // two players can be bound to it.
                None if public_room(tx, &request.room_id)? => return Err(not_reported()),
                None => insert(tx, upload, sides, RoomKind::Private, None, now)?,
            }
        }
        Some(mut game) => {
            if game.fighters != upload.info.fighters {
                return Err(not_your_game());
            }
            if game.sides[side].account.as_deref() == Some(account) {
                // The same player again, or a public game's second player
                // sending their own side.
                if !game.sides[side].uploaded {
                    let own = own_side(upload);
                    game.sides[side].name = own.name;
                    game.sides[side].country = own.country;
                    game.sides[side].uploaded = true;
                }
            } else if !game.published
                && game.sides[side].account.is_none()
                && game.sides[other].account.as_deref() != Some(account)
            {
                // The other player of a private room's game.
                game.sides[side] = own_side(upload);
                game.kind = RoomKind::Private;
                game.winner = None;
                publish(tx, &mut game, show, now)?;
            } else {
                return Err(not_your_game());
            }
            game
        }
    };
    save(tx, &game)?;
    let mut shared = true;
    for known in game.sides.iter().filter_map(|side| side.account.as_deref()) {
        shared &= sharing_on(tx, known)?;
    }
    let state = if !shared {
        UploadState::NotShared
    } else if game.published {
        UploadState::Published
    } else {
        UploadState::Held
    };
    Ok(UploadAnswer {
        state,
        id: (state == UploadState::Published).then_some(game.id),
    })
}

/// Records a game a public room host reported, between the Discord accounts
/// of `winner` and `loser`. An upload of a public room's game is taken only
/// once its game is recorded here (`upload`), so nothing is listed from here.
pub fn reported(
    tx: &Transaction<'_>,
    room_id: &str,
    table: u8,
    generation: u64,
    (winner, loser): (&EmberId, &EmberId),
    now: u64,
) -> Result<()> {
    let (Some(winner), Some(loser)) = (account(tx, winner)?, account(tx, loser)?) else {
        return Ok(());
    };
    if winner != loser {
        tx.execute(
            "INSERT OR IGNORE INTO channel_games (room_id, table_id, generation, winner, loser, played_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![room_id, table, generation, winner, loser, now],
        )?;
    }
    Ok(())
}

/// The caller's choice to share their games.
pub fn sharing(tx: &Transaction<'_>, ember_id: &EmberId) -> Result<ReplaySharing> {
    let Some(account) = account(tx, ember_id)? else {
        return Ok(ReplaySharing::NoAccount);
    };
    Ok(if sharing_on(tx, &account)? {
        ReplaySharing::On
    } else {
        ReplaySharing::Off
    })
}

/// Stores the choice of the player as `ember_id` and answers it.
pub fn choose(
    tx: &Transaction<'_>,
    ember_id: &EmberId,
    sharing: bool,
    now: u64,
) -> Result<ReplaySharing> {
    let account = account(tx, ember_id)?.ok_or_else(|| {
        refuse(
            NO_ACCOUNT,
            "Connect a Discord account to choose whether to share replays.",
        )
    })?;
    tx.execute(
        "INSERT INTO replay_sharing (discord_user_id, sharing, changed_at) VALUES (?1, ?2, ?3)
         ON CONFLICT (discord_user_id) DO UPDATE SET sharing = ?2, changed_at = ?3",
        params![account, sharing, now],
    )?;
    Ok(if sharing {
        ReplaySharing::On
    } else {
        ReplaySharing::Off
    })
}

/// Part of a game's listing that both players share.
const LISTED: &str = "r.published = 1
    AND NOT EXISTS (SELECT 1 FROM replay_sharing s
                    WHERE s.sharing = 0 AND s.discord_user_id IN (r.account0, r.account1))";

/// A `LIKE` pattern matching names that contain `text`, whatever its case.
fn contains(text: &str) -> String {
    let mut pattern = String::from("%");
    for ch in text.chars() {
        if matches!(ch, '%' | '_' | '\\') {
            pattern.push('\\');
        }
        pattern.push(ch);
    }
    pattern.push('%');
    pattern
}

/// A page of listed replays, newest first, as the player as `caller` sees it
/// (their own sides are marked), or as a web page does without one.
pub fn list(
    tx: &Transaction<'_>,
    caller: Option<&EmberId>,
    query: &ListQuery,
) -> Result<ReplayPage> {
    let own = match caller {
        Some(ember_id) => account(tx, ember_id)?,
        None => None,
    };
    let mut sql = format!(
        "SELECT r.id, r.played_at, r.room_kind, r.winner, r.size,
                r.name0, r.country0, r.fighter0, r.badge0, r.account0,
                r.name1, r.country1, r.fighter1, r.badge1, r.account1
         FROM channel_replays r WHERE {LISTED}"
    );
    let mut values: Vec<Value> = Vec::new();
    if let Some(fighter) = query.fighter {
        sql.push_str(" AND (r.fighter0 = ? OR r.fighter1 = ?)");
        values.extend([Value::from(fighter), Value::from(fighter)]);
    }
    if let Some(band) = query.band {
        sql.push_str(" AND (r.band0 = ? OR r.band1 = ?)");
        values.extend([Value::from(band), Value::from(band)]);
    }
    if let Some(name) = &query.name {
        sql.push_str(" AND (r.name0 LIKE ? ESCAPE '\\' OR r.name1 LIKE ? ESCAPE '\\')");
        let pattern = contains(name);
        values.extend([Value::from(pattern.clone()), Value::from(pattern)]);
    }
    if let Some(before) = &query.before {
        let after: Option<(i64, i64)> = tx
            .query_row(
                "SELECT played_at, seq FROM channel_replays WHERE id = ?1",
                [before],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?;
        let Some((played_at, seq)) = after else {
            return Ok(ReplayPage {
                replays: Vec::new(),
                more: false,
            });
        };
        sql.push_str(" AND (r.played_at, r.seq) < (?, ?)");
        values.extend([Value::from(played_at), Value::from(seq)]);
    }
    sql.push_str(" ORDER BY r.played_at DESC, r.seq DESC LIMIT ?");
    values.push(Value::from(PAGE_SIZE as i64 + 1));
    let mut statement = tx.prepare(&sql)?;
    let mut rows = statement.query(rusqlite::params_from_iter(values))?;
    let mut replays = Vec::new();
    while let Some(row) = rows.next()? {
        let side = |first: usize| -> rusqlite::Result<ChannelSide> {
            let account: Option<String> = row.get(first + 4)?;
            Ok(ChannelSide {
                name: row.get(first)?,
                country: row.get(first + 1)?,
                fighter: row.get(first + 2)?,
                badge: badge_of_text(row.get(first + 3)?),
                own: own.is_some() && account == own,
            })
        };
        replays.push(ChannelReplay {
            id: row.get(0)?,
            played_at: row.get(1)?,
            room: kind_of(&row.get::<_, String>(2)?),
            winner: row.get(3)?,
            size: row.get(4)?,
            sides: [side(5)?, side(10)?],
        });
    }
    let more = replays.len() > PAGE_SIZE;
    replays.truncate(PAGE_SIZE);
    Ok(ReplayPage { replays, more })
}

/// The file behind the listed replay `id`, by its sha256.
pub fn file_of(tx: &Transaction<'_>, id: &str) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            &format!("SELECT r.sha256 FROM channel_replays r WHERE r.id = ?1 AND {LISTED}"),
            [id],
            |row| row.get(0),
        )
        .optional()?)
}

/// Every file some replay still names.
pub fn files_kept(tx: &Transaction<'_>) -> Result<HashSet<String>> {
    let mut statement = tx.prepare("SELECT DISTINCT sha256 FROM channel_replays")?;
    let kept = statement
        .query_map([], |row| row.get(0))?
        .collect::<rusqlite::Result<HashSet<String>>>()?;
    Ok(kept)
}

/// Forgets reported games after 14 days, uploads that found no partner after
/// two, and the oldest replays past `max_rows` or `max_bytes`, held or
/// listed, since both keep a file. The files no replay names any more are
/// left to `routes::replays::sweep`.
pub fn prune(tx: &Transaction<'_>, max_rows: i64, max_bytes: i64, now: u64) -> Result<()> {
    tx.execute(
        "DELETE FROM channel_games WHERE played_at < ?1",
        [now.saturating_sub(GAME_SECS)],
    )?;
    tx.execute(
        "DELETE FROM channel_replays WHERE published = 0 AND created_at < ?1",
        [now.saturating_sub(HELD_SECS)],
    )?;
    let (mut rows, mut bytes): (i64, i64) = tx.query_row(
        "SELECT COUNT(*), COALESCE(SUM(size), 0) FROM channel_replays",
        [],
        |row| Ok((row.get(0)?, row.get(1)?)),
    )?;
    if rows <= max_rows && bytes <= max_bytes {
        return Ok(());
    }
    let mut oldest = Vec::new();
    {
        let mut statement =
            tx.prepare("SELECT seq, size FROM channel_replays ORDER BY played_at, seq")?;
        let mut listed = statement.query([])?;
        while rows > max_rows || bytes > max_bytes {
            let Some(row) = listed.next()? else { break };
            oldest.push(row.get::<_, i64>(0)?);
            rows -= 1;
            bytes -= row.get::<_, i64>(1)?;
        }
    }
    for seq in oldest {
        tx.execute("DELETE FROM channel_replays WHERE seq = ?1", [seq])?;
    }
    Ok(())
}

/// The Discord account connected to `ember_id`, if any.
pub fn uploader(tx: &Transaction<'_>, ember_id: &EmberId) -> Result<Option<String>> {
    account(tx, ember_id)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::Db;

    const NOW: u64 = 10_000_000;

    fn add(
        tx: &Transaction<'_>,
        generation: u64,
        played_at: u64,
        published: bool,
        created_at: u64,
    ) {
        tx.execute(
            "INSERT INTO channel_replays
                 (id, room_id, table_id, generation, room_kind, uploaded0, uploaded1,
                  fighter0, fighter1, played_at, sha256, size, published, created_at)
             VALUES (?1, 'room', 0, ?2, 'public', 0, 0, 1, 2, ?3, 'sha', 100, ?4, ?5)",
            params![
                format!("{generation:032x}"),
                generation,
                played_at,
                published,
                created_at
            ],
        )
        .unwrap();
    }

    fn kept(tx: &Transaction<'_>) -> Vec<u64> {
        let mut statement = tx
            .prepare("SELECT played_at FROM channel_replays ORDER BY played_at")
            .unwrap();
        statement
            .query_map([], |row| row.get(0))
            .unwrap()
            .collect::<rusqlite::Result<_>>()
            .unwrap()
    }

    #[test]
    fn a_name_filter_matches_text_and_not_patterns() {
        assert_eq!(contains("Ab"), "%Ab%");
        assert_eq!(contains(r"a%b_c\"), r"%a\%b\_c\\%");
    }

    #[tokio::test]
    async fn the_oldest_replays_go_first_past_either_cap() {
        let db = Db::open(std::path::Path::new(":memory:")).unwrap();
        db.write(|tx| {
            for at in 10..15 {
                add(tx, at, at, true, NOW);
            }
            // A held upload that found no partner in two days, and a fresh one.
            add(tx, 20, 20, false, NOW - 3 * 24 * 60 * 60);
            add(tx, 21, 21, false, NOW - 60);
            tx.execute(
                "INSERT INTO channel_games (room_id, table_id, generation, winner, loser, played_at)
                 VALUES ('room', 0, 1, 'a', 'b', ?1), ('room', 0, 2, 'a', 'b', ?2)",
                params![NOW - 15 * 24 * 60 * 60, NOW - 60],
            )?;
            // The fresh held upload counts toward the caps like the rest.
            prune(tx, 4, i64::MAX, NOW)?;
            assert_eq!(kept(tx), vec![12, 13, 14, 21]);
            let games: i64 =
                tx.query_row("SELECT COUNT(*) FROM channel_games", [], |r| r.get(0))?;
            assert_eq!(games, 1);
            // 100 bytes each: 250 holds two.
            prune(tx, i64::MAX, 250, NOW)?;
            assert_eq!(kept(tx), vec![14, 21]);
            Ok(())
        })
        .await
        .unwrap();
    }
}
