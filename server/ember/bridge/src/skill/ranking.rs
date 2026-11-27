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
        Board, BoardEntry, CHOICE_TOO_SOON, Listing, NO_ACCOUNT, RankingChoice, SkillBadge,
        Standing,
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
                 WHERE game = 'usf4' AND discord_user_id = ?1",
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
         ON CONFLICT (game, discord_user_id) DO UPDATE SET ranked = ?2, changed_at = ?3,
             name = ?4, country = ?5",
        params![account, chosen.ranked, changed_at, name, country],
    )?;
    super::board_moved();
    standing(tx, ember_id, classes_shown, developers, now)
}

/// An account that takes part in ranking and sent the name the boards list
/// it under: who the class board and the points boards are made of.
pub struct Listed {
    pub account: String,
    /// The Ember ID its Discord account is connected to.
    pub ember_id: String,
    pub listing: Listing,
    /// The class the boards show it with today. None while its rating is
    /// placing or faint (not played lately; it comes back after a few
    /// games), and for a developer, whose mark stands in place of a class.
    pub class: Option<Class>,
}

/// A listed account's class today.
#[derive(Clone, Copy)]
pub struct Class {
    /// The band its next ticket would show (`shown_band`), so a player who
    /// settled since their last ticket has it already.
    pub band: u8,
    /// Its last rated game.
    pub played_at: u64,
}

/// Every listed account, in no order. Read only, so a page anyone can open
/// never moves a stored class.
pub(super) fn listed(tx: &Transaction<'_>, developers: &[String], now: u64) -> Result<Vec<Listed>> {
    let mut statement = tx.prepare(
        "SELECT c.discord_user_id, d.ember_id, c.name, c.country,
                r.rating, r.deviation, r.volatility, r.played_at, r.band
         FROM ranking_choices c
         JOIN discord_accounts d ON d.user_id = c.discord_user_id
         LEFT JOIN skill_ratings r ON r.game = c.game AND r.discord_user_id = c.discord_user_id
         WHERE c.game = 'usf4' AND c.ranked AND c.name IS NOT NULL",
    )?;
    let mut rows = statement.query([])?;
    let mut listed = Vec::new();
    while let Some(row) = rows.next()? {
        let account: String = row.get(0)?;
        // Without a rated game the join has no rating to read.
        let class = match row.get::<_, Option<u64>>(7)? {
            Some(played_at) if !developers.contains(&account) => {
                let rating = Rating {
                    rating: row.get(4)?,
                    deviation: row.get(5)?,
                    volatility: row.get(6)?,
                };
                (aged(rating, played_at, now).deviation <= SETTLED_DEVIATION).then_some(Class {
                    band: shown_band(row.get(8)?, rating.rating),
                    played_at,
                })
            }
            _ => None,
        };
        listed.push(Listed {
            account,
            ember_id: row.get(1)?,
            listing: Listing {
                name: row.get(2)?,
                country: row.get(3)?,
            },
            class,
        });
    }
    Ok(listed)
}

/// The board as the player as `ember_id` sees it, or as a web page sees it
/// without one (no entry is `own`): every listed account with a class
/// (`Listed::class`), the highest first. Within a class the most recent to
/// play comes first, so the board is about who plays. It lists at most `most`
/// players, so a cap drops the lowest classes first; the caller's own entry is
/// always on it. No rating, position or number is on it.
pub fn board(
    tx: &Transaction<'_>,
    ember_id: Option<&EmberId>,
    developers: &[String],
    now: u64,
    most: usize,
) -> Result<Board> {
    let mut listed: Vec<(u64, BoardEntry)> = listed(tx, developers, now)?
        .into_iter()
        .filter_map(|player| {
            let class = player.class?;
            let entry = BoardEntry {
                band: class.band,
                own: ember_id.is_some_and(|own| own.as_str() == player.ember_id),
                listing: player.listing,
            };
            Some((class.played_at, entry))
        })
        .collect();
    listed.sort_by(|(one_at, one), (other_at, other)| {
        other
            .band
            .cmp(&one.band)
            .then(other_at.cmp(one_at))
            .then_with(|| one.listing.name.cmp(&other.listing.name))
    });
    // Lower classes come last, so a cap leaves them off. The caller's own
    // entry stays in place of the last one: it is no higher than that one,
    // so the board is still in class order.
    let own = listed.iter().position(|(_, entry)| entry.own);
    let mut players: Vec<BoardEntry> = Vec::with_capacity(listed.len().min(most));
    for (index, (_, entry)) in listed.into_iter().enumerate() {
        if index < most {
            players.push(entry);
        } else if Some(index) == own {
            if let Some(last) = players.last_mut() {
                *last = entry;
            }
            break;
        }
    }
    Ok(Board { players })
}

/// The class the public board shows today for every player on it, by Ember
/// ID: the same players and classes as `board`, without its cap. The
/// archive's games carry these (`archive::classes`).
pub fn public_classes(
    tx: &Transaction<'_>,
    developers: &[String],
    now: u64,
) -> Result<std::collections::HashMap<String, u8>> {
    Ok(listed(tx, developers, now)?
        .into_iter()
        .filter_map(|player| Some((player.ember_id, player.class?.band)))
        .collect())
}

fn refuse(reason: &'static str, code: ErrorCode, message: &'static str) -> ApiFailure {
    ApiFailure::new(code, message).detail("reason", reason)
}
