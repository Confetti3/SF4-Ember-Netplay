//! A tournament set's games in the archive. Each comes in as the set holds
//! it (`match_replays`), sided by the set's roster, never by what a sender
//! says: from the set's replay route (`routes::replays::put`), and from
//! `backfill` for the games sets kept before the archive was on, which no
//! player's game sends again. Each game a set gains in the archive makes
//! its pack due again (`delivery::packs`).
use ember_protocol::{EmberId, replays::Replay, rooms::is_country};
use rusqlite::{Transaction, params};
use serde_json::Value;

use super::{Admission, Attribution, Refused, add_choices, admit};
use crate::{
    ctx::Ctx,
    error::Result,
    routes::{
        ledger::{Match, load_listed},
        replays::described,
    },
};

/// Sets' games the backfill takes in per maintenance pass.
const BACKFILLED_PER_PASS: i64 = 16;

/// Whether a set's pack in the bucket is older than its archived games, as
/// SQL over its `matches` row. The set's replays are the pack's only
/// source, so replay retention keeps them, whether the bridge has an archive
/// now or not (`routes::replays::prune`).
pub const PACK_DUE: &str = "EXISTS (SELECT 1 FROM archive_sets s
    WHERE s.match_id = matches.id AND s.uploaded_version < s.version)";

/// Whether a set holds games the archive has not taken in yet, as SQL over
/// its `matches` row. Replay retention keeps those while the bridge has an
/// archive to take them.
pub const NOT_ARCHIVED: &str =
    "EXISTS (SELECT 1 FROM match_replays r WHERE r.match_id = matches.id AND r.archived = 0)";

/// What the archive says of a set's game: the set, its event, round and
/// phase, its players' names on the event's site, and the sending player's
/// own country, each on the player's roster slot.
pub struct SetGame {
    pub match_id: String,
    pub(super) event: Option<String>,
    pub(super) round: Option<String>,
    pub(super) phase: Option<String>,
    pub(super) names: [Option<String>; 2],
    pub(super) countries: [Option<String>; 2],
}

impl SetGame {
    fn of(
        tx: &Transaction<'_>,
        found: &Match,
        sender: &EmberId,
        country: Option<&str>,
    ) -> Result<Self> {
        let described = described(tx, found)?;
        let text = |key: &str| {
            described
                .metadata
                .get(key)
                .and_then(Value::as_str)
                .map(str::to_owned)
        };
        let slot = |slot: u8| {
            described
                .players
                .iter()
                .position(|player| player.slot == slot)
        };
        let country = country.filter(|code| is_country(code));
        Ok(Self {
            match_id: found.id.clone(),
            event: described.tournament.clone(),
            round: text("round_label"),
            phase: text("phase"),
            names: [0, 1].map(|at| slot(at).and_then(|index| described.names[index].clone())),
            countries: [0, 1].map(|at| {
                country
                    .filter(|_| {
                        slot(at).is_some_and(|index| described.players[index].ember_id == *sender)
                    })
                    .map(str::to_owned)
            }),
        })
    }

    /// What this set's copy adds to game `digest` the archive already
    /// holds, first sent by `sent_by` for the set `set`, when `sender` sends
    /// it now. The game's own sender's online copy joins the set; nobody
    /// else's does. A game of this set gains its player's own country.
    pub(super) fn held(
        &self,
        tx: &Transaction<'_>,
        now: u64,
        digest: &str,
        sent_by: &str,
        set: Option<&str>,
        sender: &EmberId,
    ) -> Result<()> {
        match set {
            None if sent_by == sender.as_str() => {
                tx.execute(
                    "UPDATE archive_games SET priority = 0, match_id = ?2, event_title = ?3, round_label = ?4,
                        phase = ?5, name_a = ?6, name_b = ?7, country_a = ?8, country_b = ?9
                     WHERE digest = ?1",
                    params![
                        digest,
                        self.match_id,
                        self.event,
                        self.round,
                        self.phase,
                        self.names[0],
                        self.names[1],
                        self.countries[0],
                        self.countries[1]
                    ],
                )?;
                crate::delivery::packs::changed(tx, &self.match_id, now)?;
            }
            Some(id) if id == self.match_id && self.countries.iter().any(Option::is_some) => {
                tx.execute(
                    "UPDATE archive_games SET country_a = COALESCE(country_a, ?2), country_b = COALESCE(country_b, ?3)
                     WHERE digest = ?1",
                    params![digest, self.countries[0], self.countries[1]],
                )?;
            }
            _ => return Ok(()),
        }
        add_choices(tx, digest)
    }
}

/// Puts set `found`'s game `crc`, as the set holds it, in the archive, sent
/// by `sender`, one of its players, with their own `country`, and marks it
/// `archived`. Nothing when the bridge has no archive, and nothing but the
/// mark when the set's copy is not a whole replay (one kept before the
/// bridge read replays as the game does). The refusal, if the archive has
/// no room for it now; the game stays unmarked then.
pub fn keep(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    found: &Match,
    crc: &str,
    sender: &EmberId,
    country: Option<&str>,
) -> Result<Option<Refused>> {
    if ctx.config.archive.is_none() {
        return Ok(None);
    }
    let file: Vec<u8> = tx.query_row(
        "SELECT replay FROM match_replays WHERE match_id = ?1 AND crc = ?2",
        params![found.id, crc],
        |row| row.get(0),
    )?;
    if let Some(replay) = Replay::parse(&file) {
        let game = SetGame::of(tx, found, sender, country)?;
        if let Admission::Refused(refused) =
            admit(tx, ctx.now, &file, &replay, sender, Attribution::Set(&game))?
        {
            return Ok(Some(refused));
        }
    } else {
        eprintln!(
            "archive: set {} holds {crc}, which is not a whole replay",
            found.id
        );
    }
    archived(tx, &found.id, crc)?;
    Ok(None)
}

/// Marks set `match_id`'s game `crc` as one the archive has taken in.
fn archived(tx: &Transaction<'_>, match_id: &str, crc: &str) -> Result<()> {
    tx.execute(
        "UPDATE match_replays SET archived = 1 WHERE match_id = ?1 AND crc = ?2",
        params![match_id, crc],
    )?;
    Ok(())
}

/// Takes the next sets' games the archive has not taken in yet (not
/// `archived`), oldest first, as their first sender sent them. A game the
/// archive has no room for stops the pass there, for the next pass to try
/// again. Runs with the bridge's maintenance when it has an archive.
pub fn backfill(tx: &Transaction<'_>, ctx: &Ctx) -> Result<()> {
    if ctx.config.archive.is_none() {
        return Ok(());
    }
    let games = tx
        .prepare(
            "SELECT match_id, crc, sent_by FROM match_replays
             WHERE archived = 0 ORDER BY received_at, match_id, crc LIMIT ?1",
        )?
        .query_map([BACKFILLED_PER_PASS], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, String>(2)?,
            ))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for (match_id, crc, sent_by) in games {
        match (load_listed(tx, &match_id)?, EmberId::parse(&sent_by)) {
            (Some(found), Ok(sender)) => {
                if keep(tx, ctx, &found, &crc, &sender, None)?.is_some() {
                    break;
                }
            }
            // Nothing the archive could say of it; passed over for good.
            _ => archived(tx, &match_id, &crc)?,
        }
    }
    Ok(())
}
