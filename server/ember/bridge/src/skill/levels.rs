//! Points (docs/design/FIGHTER_LEVELS.md): for each fighter a Discord account
//! has played in counted public room games, its games and wins (FP), and for
//! the account itself every counted game whether or not it named fighters
//! (BP). The points, games plus wins, are a grind value, never skill: they
//! only go up, and nothing of the rating is read to make them. `record_games`
//! decides which games count; this module stores them and serves them.
//!
//! Only games and wins are stored. The tier thresholds (`TIER_STARTS` for a
//! fighter, `ACCOUNT_TIER_STARTS` for the account) live here alone and are
//! applied when points are served, so they can be retuned without a data
//! change or a client update. A card is public: it shows these points to
//! anyone with the player's card token, whether or not the player takes part
//! in ranking.
//!
//! The milestone awards (`src/netplay/Awards.hxx`) read the same counts, as
//! one `AwardCounts`. What they need beyond them, the best run of wins and the
//! highest class ever shown, is kept in `award_counters` and only goes up; the
//! rival and opponent counts are read from the pair records. A player's own
//! answer carries all of them exactly, and their card as `AwardCounts::shown`
//! has them.
use ember_protocol::{
    EmberId,
    levels::{
        AwardCounts, CardAccount, CardFighter, MAX_TIER, OwnAccount, OwnFighter, OwnLevels,
        PlayerCard, Relation,
    },
};
use rusqlite::{OptionalExtension, Transaction, params};

use super::account;
use crate::error::Result;

/// The points where each fighter tier begins, Ash to Ember: the frame colours
/// of `v2::TierColor`.
const TIER_STARTS: [u64; MAX_TIER as usize + 1] = [0, 200, 700, 1500, 2500];

/// The account's own thresholds, for the points of every counted game.
const ACCOUNT_TIER_STARTS: [u64; MAX_TIER as usize + 1] = [0, 500, 2000, 5000, 10000];

/// Where the tiers of one kind of points begin.
struct Tiers([u64; MAX_TIER as usize + 1]);

// The protocol names the last tier and clients draw one frame colour for each,
// so a table has one start per tier (its type), begins at 0 and only goes up.
// A table that does not is a build error, not a points answer every client
// refuses.
const fn tiers(starts: [u64; MAX_TIER as usize + 1]) -> Tiers {
    assert!(starts[0] == 0);
    let mut at = 1;
    while at < starts.len() {
        assert!(starts[at - 1] < starts[at]);
        at += 1;
    }
    Tiers(starts)
}

const FIGHTER_TIERS: Tiers = tiers(TIER_STARTS);
const ACCOUNT_TIERS: Tiers = tiers(ACCOUNT_TIER_STARTS);

impl Tiers {
    /// The tier `points` reach, 0 to `MAX_TIER`.
    fn tier(&self, points: u64) -> u8 {
        self.0
            .iter()
            .rposition(|start| points >= *start)
            .unwrap_or(0) as u8
    }

    /// The points where `tier`, a result of `tier()`, begins.
    fn floor(&self, tier: u8) -> u64 {
        self.0[usize::from(tier)]
    }

    /// The points where the tier after `tier` begins; none at the last tier.
    fn next(&self, tier: u8) -> Option<u64> {
        self.0.get(usize::from(tier) + 1).copied()
    }
}

/// One count of games and wins: a fighter's or the account's.
struct Counted {
    games: u64,
    wins: u64,
    /// When the first game was counted; none before one.
    first_at: Option<u64>,
}

impl Counted {
    fn points(&self) -> u64 {
        self.games.saturating_add(self.wins)
    }

    /// The account's own answer: its points and frame, with `awards`, which
    /// hold the games and wins these points sum.
    fn own_account(&self, awards: AwardCounts) -> OwnAccount {
        let points = self.points();
        let tier = ACCOUNT_TIERS.tier(points);
        OwnAccount {
            points,
            awards,
            tier,
            floor: ACCOUNT_TIERS.floor(tier),
            next: ACCOUNT_TIERS.next(tier),
            since: self.first_at,
        }
    }

    /// The points and the frame; the games and wins behind them are in the
    /// card's `awards`.
    fn card_account(&self) -> CardAccount {
        let points = self.points();
        CardAccount {
            points,
            tier: ACCOUNT_TIERS.tier(points),
        }
    }

    fn own_fighter(&self, fighter: u8) -> OwnFighter {
        let points = self.points();
        let tier = FIGHTER_TIERS.tier(points);
        OwnFighter {
            fighter,
            points,
            games: self.games,
            wins: self.wins,
            tier,
            floor: FIGHTER_TIERS.floor(tier),
            next: FIGHTER_TIERS.next(tier),
            since: self.first_at.unwrap_or_default(),
        }
    }

    fn card_fighter(&self, fighter: u8) -> CardFighter {
        let points = self.points();
        CardFighter {
            fighter,
            points,
            tier: FIGHTER_TIERS.tier(points),
        }
    }
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

/// Counts a game for the account itself, won or not, whether or not the game
/// named fighters.
pub(super) fn count_account(
    tx: &Transaction<'_>,
    account: &str,
    won: bool,
    now: u64,
) -> Result<()> {
    tx.execute(
        "INSERT INTO account_points (discord_user_id, games, wins, first_at, played_at)
         VALUES (?1, 1, ?2, ?3, ?3)
         ON CONFLICT (discord_user_id) DO UPDATE SET games = games + 1,
             wins = wins + ?2, played_at = ?3",
        params![account, u32::from(won), now],
    )?;
    Ok(())
}

/// Counts the result of a game in `account`'s run of wins: a win goes on with
/// it, a loss ends it, and the best run so far only ever rises.
pub(super) fn count_streak(
    tx: &Transaction<'_>,
    account: &str,
    won: bool,
) -> Result<()> {
    tx.execute(
        "INSERT INTO award_counters (discord_user_id, streak, best_streak)
         VALUES (?1, ?2, ?2)
         ON CONFLICT (discord_user_id) DO UPDATE SET
             streak = CASE WHEN ?2 = 1 THEN streak + 1 ELSE 0 END,
             best_streak = MAX(best_streak, CASE WHEN ?2 = 1 THEN streak + 1 ELSE 0 END)",
        params![account, u32::from(won)],
    )?;
    Ok(())
}

/// Remembers that `account` showed class `band` (1 to the top): its peak only
/// rises, so an award for a class is not taken away when the class drops.
pub(super) fn note_class(tx: &Transaction<'_>, account: &str, band: u8) -> Result<()> {
    if band == 0 {
        return Ok(());
    }
    tx.execute(
        "INSERT INTO award_counters (discord_user_id, peak_band) VALUES (?1, ?2)
         ON CONFLICT (discord_user_id) DO UPDATE SET peak_band = MAX(peak_band, ?2)",
        params![account, band],
    )?;
    Ok(())
}

/// Every award counter of `account`, exactly, with the games and wins of
/// `counted`: its best run of wins, its highest class ever shown, the most
/// games against one opponent and the opponents faced (from the pair records,
/// kept from migration 060 on). The class now is read too, so a peak the
/// counters missed (a bridge from before them showed it) is not lower than the
/// class itself.
fn award_counts(tx: &Transaction<'_>, account: &str, counted: &Counted) -> Result<AwardCounts> {
    let (best_streak, peak): (u32, u8) = tx
        .query_row(
            "SELECT best_streak, peak_band FROM award_counters WHERE discord_user_id = ?1",
            [account],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?
        .unwrap_or((0, 0));
    let shown: u8 = tx
        .query_row(
            "SELECT band FROM skill_ratings WHERE game = 'usf4' AND discord_user_id = ?1",
            [account],
            |row| row.get(0),
        )
        .optional()?
        .unwrap_or(0);
    let (rival, opponents) = tx.query_row(
        "SELECT COALESCE(MAX(first_wins + second_wins), 0), COUNT(*) FROM pair_records
         WHERE first = ?1 OR second = ?1",
        [account],
        |row| Ok((row.get(0)?, row.get(1)?)),
    )?;
    Ok(AwardCounts {
        games: counted.games,
        wins: counted.wins,
        best_streak,
        peak_class: peak.max(shown),
        rival,
        opponents,
    })
}

/// The points of the player as `ember_id`, for themselves: the account's and
/// every fighter's.
pub fn own(tx: &Transaction<'_>, ember_id: &EmberId) -> Result<OwnLevels> {
    let Some(account) = account(tx, ember_id)? else {
        return Ok(OwnLevels::NoAccount {});
    };
    let counted = account_count(tx, &account)?;
    Ok(OwnLevels::Counted {
        account: counted.own_account(award_counts(tx, &account, &counted)?),
        fighters: fighter_counts(tx, &account)?
            .iter()
            .map(|(fighter, count)| count.own_fighter(*fighter))
            .collect(),
        // The route that knows the keys and the classes fills it in.
        rival: None,
    })
}

/// How many of `account`'s pairs the most played opponent is picked from.
pub const RIVAL_CANDIDATES: usize = 5;

/// One pair `account` is in, from its own side.
pub struct Pair {
    /// The other Discord account.
    pub other: String,
    /// The games `account` won against it.
    pub won: u64,
    /// The games it won against `account`.
    pub lost: u64,
}

/// Up to `limit` of `account`'s pairs that played a counted game, the most
/// games first and, among as many, the latest played first. A pair row keeps
/// `account` as its first or its second, so the two are read through their own
/// keys (the primary key, and `pair_records_second` from migration 063) and
/// turned to `account`'s side.
pub fn pairs_by_games(tx: &Transaction<'_>, account: &str, limit: usize) -> Result<Vec<Pair>> {
    let mut statement = tx.prepare(
        "SELECT other, won, lost FROM (
             SELECT second AS other, first_wins AS won, second_wins AS lost, played_at
             FROM pair_records WHERE first = ?1
             UNION ALL
             SELECT first, second_wins, first_wins, played_at
             FROM pair_records WHERE second = ?1)
         WHERE won + lost > 0 AND other <> ?1
         ORDER BY won + lost DESC, played_at DESC, other
         LIMIT ?2",
    )?;
    let rows = statement
        .query_map(params![account, limit as i64], |row| {
            Ok(Pair {
                other: row.get(0)?,
                won: row.get(1)?,
                lost: row.get(2)?,
            })
        })?
        .collect::<rusqlite::Result<_>>()?;
    Ok(rows)
}

/// The card of Discord account `account` as another player sees it: its
/// points, always (FIGHTER_LEVELS.md, "Cards are public"), and None once it
/// has no Discord account connected. Its relation is `none`; the card look-up
/// sets the caller's own (`cards::card_for`). A friends state, which wants the
/// points and fighters alone, reads this one.
pub fn card(tx: &Transaction<'_>, account: &str) -> Result<Option<PlayerCard>> {
    card_with(tx, account, false)
}

/// `card` with the award counters a card shows (`AwardCounts::shown`): the
/// class only while the player takes part in ranking.
pub fn awarded_card(tx: &Transaction<'_>, account: &str) -> Result<Option<PlayerCard>> {
    card_with(tx, account, true)
}

fn card_with(tx: &Transaction<'_>, account: &str, awards: bool) -> Result<Option<PlayerCard>> {
    let connected: bool = tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM discord_accounts WHERE user_id = ?1)",
        [account],
        |row| row.get(0),
    )?;
    if !connected {
        return Ok(None);
    }
    let counted = account_count(tx, account)?;
    let awards = if awards {
        award_counts(tx, account, &counted)?.shown(super::takes_part(tx, account)?)
    } else {
        AwardCounts::default()
    };
    Ok(Some(PlayerCard {
        fighters: fighter_counts(tx, account)?
            .iter()
            .map(|(fighter, count)| count.card_fighter(*fighter))
            .collect(),
        awards,
        ..PlayerCard::bare(counted.card_account(), Relation::None)
    }))
}

/// `account`'s own count; none counted yet reads as no games.
fn account_count(tx: &Transaction<'_>, account: &str) -> Result<Counted> {
    let stored = tx
        .query_row(
            "SELECT games, wins, first_at FROM account_points WHERE discord_user_id = ?1",
            [account],
            |row| {
                Ok(Counted {
                    games: row.get(0)?,
                    wins: row.get(1)?,
                    first_at: Some(row.get(2)?),
                })
            },
        )
        .optional()?;
    Ok(stored.unwrap_or(Counted {
        games: 0,
        wins: 0,
        first_at: None,
    }))
}

/// `account`'s played fighters with their counts, ascending.
fn fighter_counts(tx: &Transaction<'_>, account: &str) -> Result<Vec<(u8, Counted)>> {
    let mut statement = tx.prepare(
        "SELECT fighter, games, wins, first_at FROM fighter_levels
         WHERE discord_user_id = ?1 ORDER BY fighter",
    )?;
    let rows = statement
        .query_map([account], |row| {
            Ok((
                row.get(0)?,
                Counted {
                    games: row.get(1)?,
                    wins: row.get(2)?,
                    first_at: Some(row.get(3)?),
                },
            ))
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
            assert_eq!(FIGHTER_TIERS.tier(points), want, "{points} points");
        }
    }

    #[test]
    fn an_account_tier_starts_at_its_own_threshold() {
        for (points, want) in [
            (0, 0),
            (499, 0),
            (500, 1),
            (1999, 1),
            (2000, 2),
            (4999, 2),
            (5000, 3),
            (9999, 3),
            (10_000, 4),
            (u64::MAX, 4),
        ] {
            assert_eq!(ACCOUNT_TIERS.tier(points), want, "{points} points");
        }
    }

    #[test]
    fn a_tier_floor_is_its_threshold_and_never_above_the_points() {
        for tiers in [&FIGHTER_TIERS, &ACCOUNT_TIERS] {
            for (tier, start) in tiers.0.into_iter().enumerate() {
                assert_eq!(tiers.floor(tier as u8), start);
            }
            for points in tiers
                .0
                .into_iter()
                .flat_map(|start| [start.saturating_sub(1), start, start + 1])
                .chain([1_000_000])
            {
                let tier = tiers.tier(points);
                assert!(tiers.floor(tier) <= points, "{points} points");
                assert!(
                    tiers.next(tier).is_none_or(|next| points < next),
                    "{points} points"
                );
            }
        }
    }

    #[test]
    fn the_next_tier_is_the_next_threshold_and_the_top_has_none() {
        for (tiers, thresholds) in [
            (&FIGHTER_TIERS, [200, 700, 1500, 2500]),
            (&ACCOUNT_TIERS, [500, 2000, 5000, 10_000]),
        ] {
            for (tier, start) in thresholds.into_iter().enumerate() {
                assert_eq!(tiers.next(tier as u8), Some(start));
            }
            assert_eq!(tiers.next(4), None);
        }
    }

    /// The games and wins of `counted` as an account's award counts.
    fn awards_of(counted: &Counted) -> AwardCounts {
        AwardCounts {
            games: counted.games,
            wins: counted.wins,
            ..AwardCounts::default()
        }
    }

    #[test]
    fn what_is_served_passes_the_protocols_own_checks() {
        let row = |games: u64, wins: u64| Counted {
            games,
            wins,
            first_at: Some(1_729_468_800),
        };
        for games in 1..=1_400 {
            for wins in [0, games / 2, games] {
                let counted = row(games, wins);
                let own = counted.own_fighter(7);
                assert!(own.check().is_ok(), "{own:?}");
                assert!(counted.card_fighter(7).check().is_ok());
                let account = counted.own_account(awards_of(&counted));
                assert!(account.check().is_ok(), "{account:?}");
                assert!(counted.card_account().check().is_ok());
            }
        }
        // The top tier has no next tier.
        let top = row(2_500, 1_300).own_fighter(7);
        assert_eq!((top.points, top.tier, top.floor, top.next), (3_800, 4, 2_500, None));
        let first = row(1, 0).own_fighter(7);
        assert_eq!((first.points, first.tier, first.floor, first.next), (1, 0, 0, Some(200)));
        let edge = row(100, 100).own_fighter(7);
        assert_eq!((edge.points, edge.tier, edge.floor, edge.next), (200, 1, 200, Some(700)));
        // An account with no game has no `since`, and passes the checks.
        let none = Counted {
            games: 0,
            wins: 0,
            first_at: None,
        };
        let account = none.own_account(awards_of(&none));
        assert!(account.check().is_ok(), "{account:?}");
        assert_eq!(
            (account.points, account.tier, account.floor, account.next, account.since),
            (0, 0, 0, Some(500), None)
        );
        let top_row = row(8_000, 3_000);
        let account_top = top_row.own_account(awards_of(&top_row));
        assert!(account_top.check().is_ok());
        assert_eq!(
            (account_top.tier, account_top.floor, account_top.next),
            (4, 10_000, None)
        );
    }
}
