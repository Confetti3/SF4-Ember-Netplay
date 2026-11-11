//! Fighter points (docs/design/FIGHTER_LEVELS.md): for each fighter a Discord
//! account has played in counted public room games, its games and wins. The
//! points, games plus wins, are a grind value, never skill: they only go up,
//! and nothing of the rating is read to make them. `record_games` decides
//! which games count; this module stores them and serves them.
//!
//! Only games and wins are stored. The tier thresholds (`TIER_STARTS`) live
//! here alone and are applied when points are served, so they can be retuned
//! without a data change or a client update.
use ember_protocol::{
    EmberId,
    levels::{CardFighter, OwnFighter, OwnLevels, PlayerCard, Relation},
};
use rusqlite::{Transaction, params};

use super::{account, takes_part};
use crate::error::Result;

/// The points where each tier begins, Ash to Ember: the frame colours of
/// `v2::TierColor`. The one place the thresholds are written.
const TIER_STARTS: [u64; 5] = [0, 200, 700, 1500, 2500];

/// The tier `points` reach, 0 to `TIER_STARTS.len() - 1`.
pub fn tier(points: u64) -> u8 {
    TIER_STARTS.iter().rposition(|start| points >= *start).unwrap_or(0) as u8
}

/// The points where `tier`, a result of `tier()`, begins.
pub fn floor(tier: u8) -> u64 {
    TIER_STARTS[usize::from(tier)]
}

/// The points where the tier after `tier` begins; none at the last tier.
pub fn next(tier: u8) -> Option<u64> {
    TIER_STARTS.get(usize::from(tier) + 1).copied()
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

/// The points of the player as `ember_id`, for themselves: every fighter,
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
/// points while it takes part in ranking (FIGHTER_LEVELS.md, Privacy), and
/// None once it has no Discord account connected. Its relation is `none`;
/// the card look-up sets the caller's own (`friends::store::relation`).
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
    fn points(&self) -> u64 {
        self.games.saturating_add(self.wins)
    }

    fn own(&self) -> OwnFighter {
        let points = self.points();
        let tier = tier(points);
        OwnFighter {
            fighter: self.fighter,
            points,
            games: self.games,
            wins: self.wins,
            tier,
            floor: floor(tier),
            next: next(tier),
            since: self.first_at,
        }
    }

    /// Never the wins or the games, which the points only sum.
    fn card(&self) -> CardFighter {
        let points = self.points();
        CardFighter {
            fighter: self.fighter,
            points,
            tier: tier(points),
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
    fn a_tier_starts_at_its_threshold_and_ends_one_point_before_the_next() {
        for (points, want) in [
            (0, 0),
            (199, 0),
            (200, 1),
            (699, 1),
            (700, 2),
            (1499, 2),
            (1500, 3),
            (2499, 3),
            (2500, 4),
            (1_000_000, 4),
            (u64::MAX, 4),
        ] {
            assert_eq!(tier(points), want, "{points} points");
        }
    }

    #[test]
    fn a_tier_floor_is_its_threshold_and_never_above_the_points() {
        for (tier, start) in [0, 200, 700, 1500, 2500].into_iter().enumerate() {
            assert_eq!(floor(tier as u8), start);
        }
        for points in [0, 1, 199, 200, 699, 700, 1499, 1500, 2499, 2500, 1_000_000] {
            let tier = tier(points);
            assert!(floor(tier) <= points, "{points} points");
            assert!(next(tier).is_none_or(|next| points < next), "{points} points");
        }
    }

    #[test]
    fn the_next_tier_is_the_next_threshold_and_the_top_has_none() {
        let thresholds = [200, 700, 1500, 2500];
        for (tier, start) in thresholds.into_iter().enumerate() {
            assert_eq!(next(tier as u8), Some(start));
        }
        assert_eq!(next(4), None);
    }

    #[test]
    fn what_is_served_passes_the_protocols_own_checks() {
        let row = |games: u64, wins: u64| Counted {
            fighter: 7,
            games,
            wins,
            first_at: 1_729_468_800,
        };
        for games in 1..=1_400 {
            for wins in [0, games / 2, games] {
                let counted = row(games, wins);
                let own = counted.own();
                assert!(own.check().is_ok(), "{own:?}");
                assert!(counted.card().check().is_ok());
            }
        }
        // The top tier has no next tier.
        let top = row(2_500, 1_300).own();
        assert_eq!((top.points, top.tier, top.floor, top.next), (3_800, 4, 2_500, None));
        let first = row(1, 0).own();
        assert_eq!((first.points, first.tier, first.floor, first.next), (1, 0, 0, Some(200)));
        let edge = row(100, 100).own();
        assert_eq!((edge.points, edge.tier, edge.floor, edge.next), (200, 1, 200, Some(700)));
    }
}
