//! A player's choice to take part in ranking, their own standing and the
//! ranking board (docs/design/SKILL_BADGE.md, "Rankings"). The choice and the
//! listing belong to the Discord account, like the rating.
//!
//! An account that stops taking part keeps its rating untouched: its games
//! stop counting, its class leaves tickets and the board, and its listing is
//! forgotten. Coming back resumes the rating, grown less sure by the time
//! away like any break (`load`). The choice changes at most once a day, so
//! nobody can stop after a loss and come back after a win.
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    skill::{
        Board, BoardEntry, CHOICE_TOO_SOON, Listing, MAX_BOARD, NO_ACCOUNT, RankingChoice,
        SkillBadge, Standing,
    },
};
use rusqlite::{OptionalExtension, Transaction, params};

use super::{Developers, SETTLED_DEVIATION, account, aged, badge_of, glicko::Rating, shown_band};
use crate::error::{ApiFailure, Result};

/// How long a change of the choice stands before the next.
const CHOICE_SECS: u64 = 24 * 60 * 60;

/// An account's choice as stored. Without a row it takes part, has never
/// changed and is not listed.
struct Choice {
    ranked: bool,
    changed_at: Option<u64>,
    listing: Option<Listing>,
}

impl Choice {
    fn load(tx: &Transaction<'_>, account: &str) -> Result<Self> {
        let stored = tx
            .query_row(
                "SELECT ranked, changed_at, name, country FROM ranking_choices
                 WHERE discord_user_id = ?1",
                [account],
                |row| {
                    let (name, country): (Option<String>, _) = (row.get(2)?, row.get(3)?);
                    Ok(Self {
                        ranked: row.get(0)?,
                        changed_at: row.get(1)?,
                        listing: name.map(|name| Listing { name, country }),
                    })
                },
            )
            .optional()?;
        Ok(stored.unwrap_or(Self {
            ranked: true,
            changed_at: None,
            listing: None,
        }))
    }

    /// Seconds until the choice may change again, if it may not yet.
    fn change_in(&self, now: u64) -> Option<u64> {
        self.changed_at
            .map(|changed_at| (changed_at + CHOICE_SECS).saturating_sub(now))
            .filter(|wait| *wait > 0)
    }
}

/// Where the player as `ember_id` stands. `classes_shown` is the bridge's
/// `rooms.skill_badges`: while it is off a standing carries no class. A
/// class it carries is the one the next ticket shows, and is remembered as
/// shown just as a ticket's is (`badge_of`).
pub fn standing(
    tx: &Transaction<'_>,
    ember_id: &EmberId,
    classes_shown: bool,
    developers: &[String],
    now: u64,
) -> Result<Standing> {
    let Some(account) = account(tx, ember_id)? else {
        return Ok(Standing::NoAccount);
    };
    let choice = Choice::load(tx, &account)?;
    let change_in = choice.change_in(now);
    if !choice.ranked {
        return Ok(Standing::OptedOut { change_in });
    }
    let developer = Developers {
        accounts: developers,
        shown: true,
    }
    .mark(&account);
    let badge = match (classes_shown, developer) {
        (false, _) => None,
        (true, true) => Some(SkillBadge::DEVELOPER),
        (true, false) => Some(badge_of(tx, &account, now)?),
    };
    Ok(Standing::Ranked {
        badge,
        listing: choice.listing,
        change_in,
    })
}

/// Stores the choice of the player as `ember_id` and answers where they
/// stand after it. Taking part or not changes only once its last change has
/// stood for `CHOICE_SECS`; the listing changes freely.
pub fn choose(
    tx: &Transaction<'_>,
    ember_id: &EmberId,
    chosen: RankingChoice,
    classes_shown: bool,
    developers: &[String],
    now: u64,
) -> Result<Standing> {
    let account = account(tx, ember_id)?.ok_or_else(|| {
        refuse(
            NO_ACCOUNT,
            ErrorCode::Forbidden,
            "Connect a Discord account to take part in ranking.",
        )
    })?;
    let current = Choice::load(tx, &account)?;
    let changes = chosen.ranked != current.ranked;
    if changes && let Some(wait) = current.change_in(now) {
        return Err(refuse(
            CHOICE_TOO_SOON,
            ErrorCode::StaleRevision,
            "Taking part in ranking changes at most once a day.",
        )
        .detail("change_in", wait));
    }
    let changed_at = if changes {
        Some(now)
    } else {
        current.changed_at
    };
    let (name, country) = chosen.listing.map_or((None, None), |listing| {
        (Some(listing.name), listing.country)
    });
    tx.execute(
        "INSERT INTO ranking_choices (discord_user_id, ranked, changed_at, name, country)
         VALUES (?1, ?2, ?3, ?4, ?5)
         ON CONFLICT (discord_user_id) DO UPDATE SET ranked = ?2, changed_at = ?3,
             name = ?4, country = ?5",
        params![account, chosen.ranked, changed_at, name, country],
    )?;
    standing(tx, ember_id, classes_shown, developers, now)
}

/// The board as the player as `ember_id` sees it: every listed account that
/// takes part and has a settled class, the highest first. The class is the
/// one their next ticket would show (`shown_band`), so a player who settled
/// since their last ticket is on it already. Within a class the most recent
/// to play comes first, so the board is about who plays and its cap drops
/// whoever played longest ago. No rating, position or number is on it, and
/// no developer: their mark stands in place of a class.
pub fn board(
    tx: &Transaction<'_>,
    ember_id: &EmberId,
    developers: &[String],
    now: u64,
) -> Result<Board> {
    let mut statement = tx.prepare(
        "SELECT r.rating, r.deviation, r.volatility, r.played_at, r.band, c.name, c.country,
                d.ember_id = ?1, r.discord_user_id
         FROM skill_ratings r
         JOIN ranking_choices c ON c.discord_user_id = r.discord_user_id
         JOIN discord_accounts d ON d.user_id = r.discord_user_id
         WHERE c.ranked AND c.name IS NOT NULL",
    )?;
    let mut rows = statement.query([ember_id.as_str()])?;
    let mut listed = Vec::new();
    while let Some(row) = rows.next()? {
        let rating = Rating {
            rating: row.get(0)?,
            deviation: row.get(1)?,
            volatility: row.get(2)?,
        };
        let played_at: u64 = row.get(3)?;
        // Placing, or faint: not played lately. It comes back after a few games.
        let developer = developers.contains(&row.get::<_, String>(8)?);
        if developer || aged(rating, played_at, now).deviation > SETTLED_DEVIATION {
            continue;
        }
        let entry = BoardEntry {
            band: shown_band(row.get(4)?, rating.rating),
            listing: Listing {
                name: row.get(5)?,
                country: row.get(6)?,
            },
            own: row.get(7)?,
        };
        listed.push((played_at, entry));
    }
    listed.sort_by(|(one_at, one), (other_at, other)| {
        other
            .band
            .cmp(&one.band)
            .then(other_at.cmp(one_at))
            .then_with(|| one.listing.name.cmp(&other.listing.name))
    });
    let players = listed
        .into_iter()
        .take(MAX_BOARD)
        .map(|(_, entry)| entry)
        .collect();
    Ok(Board { players })
}

fn refuse(reason: &'static str, code: ErrorCode, message: &'static str) -> ApiFailure {
    ApiFailure::new(code, message).detail("reason", reason)
}
