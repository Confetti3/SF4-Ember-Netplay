//! Fighter levels (docs/design/FIGHTER_LEVELS.md): for each fighter a Discord
//! account has played in counted public room games, its games and wins. A
//! level is a grind value, never skill: it only goes up, and nothing of the
//! rating is read to make it. `record_games` decides which games count; this
//! module stores them and serves them.
//!
//! Only games and wins are stored. The curve (`floor`, `level`) lives here
//! alone and is applied when a level is served, so it can be retuned without
//! a data change or a client update.
use ember_protocol::{
    EmberId,
    levels::{CardFighter, MAX_LEVEL, OwnFighter, OwnLevels, PlayerCard, Relation},
};
use rusqlite::{Transaction, params};

use super::{account, takes_part};
use crate::error::Result;

/// The points that reach `level`: (L - 1)(L + 3). Each step costs 2L + 3, 5
/// for the first and 121 for the last.
pub fn floor(level: u8) -> u64 {
    let level = u64::from(level);
    (level - 1) * (level + 3)
}

/// The level `points` reach, 1 to `MAX_LEVEL`. `floor(L) + 4` is `(L + 1)²`,
/// so the level is the integer square root of `points + 4`, less one.
pub fn level(points: u64) -> u8 {
    let reached = points.saturating_add(4).isqrt() - 1;
    reached.min(u64::from(MAX_LEVEL)) as u8
}

/// Counts a game for `account` on `fighter`, won or not.
pub(super) fn count(
    tx: &Transaction<'_>,
    account: &str,
    fighter: u8,
    won: bool,
    now: u64,
) -> Result<()> {
    tx.execute(
        "INSERT INTO fighter_levels (discord_user_id, fighter, games, wins, first_at, played_at)
         VALUES (?1, ?2, 1, ?3, ?4, ?4)
         ON CONFLICT (discord_user_id, fighter) DO UPDATE SET games = games + 1,
             wins = wins + ?3, played_at = ?4",
        params![account, fighter, u32::from(won), now],
    )?;
    Ok(())
}

/// The levels of the player as `ember_id`, for themselves: every fighter,
/// whether or not others see them.
pub fn own(tx: &Transaction<'_>, ember_id: &EmberId) -> Result<OwnLevels> {
    let Some(account) = account(tx, ember_id)? else {
        return Ok(OwnLevels::NoAccount {});
    };
    Ok(OwnLevels::Counted {
        shown: takes_part(tx, &account)?,
        fighters: counted(tx, &account)?.iter().map(Counted::own).collect(),
    })
}

/// The card of Discord account `account` as another player sees it: its
/// levels while it takes part in ranking (FIGHTER_LEVELS.md, Privacy), and
/// None once it has no Discord account connected. Friends are not offered
/// yet, so the relation is always `none`.
pub fn card(tx: &Transaction<'_>, account: &str) -> Result<Option<PlayerCard>> {
    let connected: bool = tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM discord_accounts WHERE user_id = ?1)",
        [account],
        |row| row.get(0),
    )?;
    if !connected {
        return Ok(None);
    }
    let fighters = if takes_part(tx, account)? {
        Some(counted(tx, account)?.iter().map(Counted::card).collect())
    } else {
        None
    };
    Ok(Some(PlayerCard {
        fighters,
        relation: Relation::None,
    }))
}

/// One stored row of `fighter_levels`.
struct Counted {
    fighter: u8,
    games: u64,
    wins: u64,
    first_at: u64,
}

impl Counted {
    fn level(&self) -> u8 {
        level(self.games.saturating_add(self.wins))
    }

    fn own(&self) -> OwnFighter {
        let level = self.level();
        OwnFighter {
            fighter: self.fighter,
            level,
            games: self.games,
            wins: self.wins,
            floor: floor(level),
            next: (level < MAX_LEVEL).then(|| floor(level + 1)),
            since: self.first_at,
        }
    }

    /// Never the wins, the points or the floor: points less games are wins.
    fn card(&self) -> CardFighter {
        CardFighter {
            fighter: self.fighter,
            level: self.level(),
            games: self.games,
        }
    }
}

/// `account`'s played fighters, ascending.
fn counted(tx: &Transaction<'_>, account: &str) -> Result<Vec<Counted>> {
    let mut statement = tx.prepare(
        "SELECT fighter, games, wins, first_at FROM fighter_levels
         WHERE discord_user_id = ?1 ORDER BY fighter",
    )?;
    let rows = statement
        .query_map([account], |row| {
            Ok(Counted {
                fighter: row.get(0)?,
                games: row.get(1)?,
                wins: row.get(2)?,
                first_at: row.get(3)?,
            })
        })?
        .collect::<rusqlite::Result<_>>()?;
    Ok(rows)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_curve_is_the_designs_table() {
        for (level, points) in [
            (1, 0),
            (2, 5),
            (13, 192),
            (25, 672),
            (37, 1440),
            (49, 2496),
            (60, 3717),
        ] {
            assert_eq!(floor(level), points, "level {level}");
        }
        assert_eq!(floor(2) - floor(1), 5);
        assert_eq!(floor(60) - floor(59), 121);
    }

    #[test]
    fn a_level_starts_at_its_floor_and_ends_one_point_before_the_next() {
        assert_eq!(level(0), 1);
        for at in 1..MAX_LEVEL {
            assert_eq!(level(floor(at)), at);
            assert_eq!(level(floor(at + 1) - 1), at);
        }
        assert_eq!(level(floor(MAX_LEVEL)), MAX_LEVEL);
        // Past the last level's floor it stays the last, for any count.
        assert_eq!(level(floor(MAX_LEVEL) + 10_000), MAX_LEVEL);
        assert_eq!(level(u64::MAX), MAX_LEVEL);
    }

    #[test]
    fn what_is_served_passes_the_protocols_own_checks() {
        let row = |games: u64, wins: u64| Counted {
            fighter: 7,
            games,
            wins,
            first_at: 1_729_468_800,
        };
        for games in 1..=2_600 {
            for wins in [0, games / 2, games] {
                let counted = row(games, wins);
                let own = counted.own();
                assert!(own.check().is_ok(), "{own:?}");
                assert!(counted.card().check().is_ok());
            }
        }
        // The top level has no next level.
        let top = row(2_500, 1_300).own();
        assert_eq!((top.level, top.next), (MAX_LEVEL, None));
        let first = row(1, 0).own();
        assert_eq!((first.level, first.floor, first.next), (1, 0, Some(5)));
    }
}
