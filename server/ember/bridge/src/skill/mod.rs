//! The hidden rating behind a player's public room class
//! (docs/design/SKILL_BADGE.md).
//!
//! Public room hosts report each game they saw won; the bridge rates it
//! here, once, between the two players' Discord accounts. An Ember ID costs
//! nothing to replace, a Discord account does, so the rating belongs to the
//! account and follows it to whichever Ember ID it is connected to. A game
//! with a fighter who has no Discord account counts for neither, and so
//! does one with a fighter whose account does not take part (`ranking`).
//! Only the class (`SkillBadge`) ever leaves the bridge: inside room tickets,
//! on the player's own standing and on the ranking board.
pub mod channel;
mod glicko;
pub mod ranking;

use ember_protocol::{
    EmberId,
    skill::{SKILL_BANDS, SkillBadge},
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Deserialize;

use crate::error::Result;
use glicko::Rating;

/// The lowest rating of each class above the first: D below 1200, D+ from
/// 1200, and so on up to Grand Master from 2050, so the 1500 every player
/// starts at is the floor of B. To be calibrated against real ratings before
/// classes show.
const BAND_FLOORS: [f64; SKILL_BANDS as usize - 1] = [
    1200.0, 1300.0, 1400.0, 1500.0, 1600.0, 1700.0, 1800.0, 1900.0, 2050.0,
];
/// How far past a class's edge a rating must go before the class shown
/// follows it, so one good or bad evening rarely changes it.
const BAND_MARGIN: f64 = 25.0;
/// The deviation at or below which a rating stands behind a class: about
/// three games for a new player, and most of a year without a game undoes it.
const SETTLED_DEVIATION: f64 = 200.0;
/// The deviation above which a rating's gap to anyone says little yet, so its
/// games count in full (`gap_weight`): about ten games for a new player.
const UNSURE_DEVIATION: f64 = 110.0;
/// One rating period a day of not playing (`Rating::idle`).
const SECS_PER_PERIOD: u64 = 24 * 60 * 60;

// How much a game counts, so playing anyone stays cheap (SKILL_BADGE.md,
// "Playing for fun"). Between settled players a game counts in full within
// FULL_WEIGHT_GAP rating points (about a class and a half) and tapers to
// MIN_WEIGHT at MIN_WEIGHT_GAP (about four and a half classes): losing a
// lopsided game costs either player little. The one exception is an upset:
// the lower-rated winner's game counts at least UPSET_WEIGHT, so beating a
// much stronger player is still worth a fair climb.
const FULL_WEIGHT_GAP: f64 = 150.0;
const MIN_WEIGHT_GAP: f64 = 450.0;
const MIN_WEIGHT: f64 = 0.1;
const UPSET_WEIGHT: f64 = 0.5;
// Long sets between two friends: past FULL_REPEATS games against each other in
// one sitting, a game counts at REPEAT_WEIGHT. A sitting ends after
// SITTING_SECS without a game between them.
const FULL_REPEATS: u32 = 8;
const REPEAT_WEIGHT: f64 = 0.25;
const SITTING_SECS: u64 = 3 * 60 * 60;

/// One game a room host saw won. The bridge reads each the supervisor passes
/// on by itself, so one it cannot read is skipped alone.
#[derive(Deserialize)]
struct Game {
    table: u8,
    generation: u64,
    winner: EmberId,
    loser: EmberId,
}

/// Rates `room_id`'s reported games that are not rated yet. A host reports
/// its recent games on every poll; each counts the first time only.
pub fn record_games(
    tx: &Transaction<'_>,
    show: channel::Show<'_>,
    room_id: &str,
    games: Option<&serde_json::Value>,
    now: u64,
) -> Result<()> {
    let games = games
        .and_then(serde_json::Value::as_array)
        .into_iter()
        .flatten();
    for game in games.filter_map(|game| Game::deserialize(game).ok()) {
        // The replay channel keeps every reported game, rated or not.
        channel::reported(
            tx,
            show,
            room_id,
            game.table,
            game.generation,
            (&game.winner, &game.loser),
            now,
        )?;
        let first = tx.execute(
            "INSERT OR IGNORE INTO rated_games (room_id, table_id, generation)
             SELECT room_id, ?2, ?3 FROM rooms WHERE room_id = ?1",
            params![room_id, game.table, game.generation],
        )? == 1;
        if !first {
            continue;
        }
        let (Some(winner), Some(loser)) = (
            rated_account(tx, &game.winner)?,
            rated_account(tx, &game.loser)?,
        ) else {
            continue;
        };
        if winner == loser {
            continue;
        }
        let (won, lost) = (load(tx, &winner, now)?.0, load(tx, &loser, now)?.0);
        let sitting = sitting_weight(tx, &winner, &loser, now)?;
        let (winner_weight, loser_weight) = weights(won, lost);
        store(
            tx,
            &winner,
            won.after(lost, 1.0, winner_weight * sitting),
            now,
        )?;
        store(
            tx,
            &loser,
            lost.after(won, 0.0, loser_weight * sitting),
            now,
        )?;
    }
    Ok(())
}

/// How much a game counts for its winner and for its loser, by their ratings:
/// the gap's weight for both, except that an upset counts at least
/// `UPSET_WEIGHT` for the winner.
fn weights(winner: Rating, loser: Rating) -> (f64, f64) {
    let gap = gap_weight(winner, loser);
    let upset = winner.rating < loser.rating;
    (if upset { gap.max(UPSET_WEIGHT) } else { gap }, gap)
}

/// How much a game between `one` and `other` counts for their rating gap.
/// A rating that is still unsure takes every game in full: its gap says
/// little, and it needs the games to settle.
fn gap_weight(one: Rating, other: Rating) -> f64 {
    if one.deviation > UNSURE_DEVIATION || other.deviation > UNSURE_DEVIATION {
        return 1.0;
    }
    let gap = (one.rating - other.rating).abs();
    let past = ((gap - FULL_WEIGHT_GAP) / (MIN_WEIGHT_GAP - FULL_WEIGHT_GAP)).clamp(0.0, 1.0);
    1.0 - past * (1.0 - MIN_WEIGHT)
}

/// Counts this game in the two accounts' current sitting and says how much it
/// counts for that. Sittings that have ended are forgotten.
fn sitting_weight(tx: &Transaction<'_>, one: &str, other: &str, now: u64) -> Result<f64> {
    let (first, second) = if one < other {
        (one, other)
    } else {
        (other, one)
    };
    let ended = now.saturating_sub(SITTING_SECS);
    tx.execute("DELETE FROM rated_pairs WHERE played_at < ?1", [ended])?;
    let games: u32 = tx.query_row(
        "INSERT INTO rated_pairs (first, second, games, played_at) VALUES (?1, ?2, 1, ?3)
         ON CONFLICT (first, second) DO UPDATE SET games = games + 1, played_at = ?3
         RETURNING games",
        params![first, second, now],
        |row| row.get(0),
    )?;
    Ok(if games > FULL_REPEATS {
        REPEAT_WEIGHT
    } else {
        1.0
    })
}

/// Ember's developers, by Discord account, and whether the build asking
/// shows their mark: one that does not is shown their class.
pub struct Developers<'a> {
    pub accounts: &'a [String],
    pub shown: bool,
}

impl Developers<'_> {
    fn mark(&self, account: &str) -> bool {
        self.shown && self.accounts.iter().any(|developer| developer == account)
    }
}

/// The badge to show for the player as `ember_id` now: none without a
/// Discord account that takes part, the developer mark for a developer, and
/// otherwise their class (`badge_of`).
pub fn show(
    tx: &Transaction<'_>,
    ember_id: &EmberId,
    developers: &Developers<'_>,
    now: u64,
) -> Result<Option<SkillBadge>> {
    let Some(account) = rated_account(tx, ember_id)? else {
        return Ok(None);
    };
    if developers.mark(&account) {
        return Ok(Some(SkillBadge::DEVELOPER));
    }
    badge_of(tx, &account, now).map(Some)
}

/// The class to show for `account` now, band 0 while it is still placing. It
/// is remembered as the class last shown, which is what the margin is
/// measured from, so the class ranges can change before anyone has seen one.
fn badge_of(tx: &Transaction<'_>, account: &str, now: u64) -> Result<SkillBadge> {
    let (rating, shown) = load(tx, account, now)?;
    let settled = rating.deviation <= SETTLED_DEVIATION;
    let band = if settled {
        shown_band(shown, rating.rating)
    } else {
        shown
    };
    if band != shown {
        tx.execute(
            "UPDATE skill_ratings SET band = ?2 WHERE discord_user_id = ?1",
            params![account, band],
        )?;
    }
    Ok(SkillBadge::class(band, band != 0 && settled))
}

/// The Discord account connected to `ember_id`.
fn account(tx: &Transaction<'_>, ember_id: &EmberId) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            "SELECT user_id FROM discord_accounts WHERE ember_id = ?1",
            [ember_id.as_str()],
            |row| row.get(0),
        )
        .optional()?)
}

/// The Discord account connected to `ember_id`, if it takes part in ranking.
fn rated_account(tx: &Transaction<'_>, ember_id: &EmberId) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            "SELECT d.user_id FROM discord_accounts d
             LEFT JOIN ranking_choices c ON c.discord_user_id = d.user_id
             WHERE d.ember_id = ?1 AND COALESCE(c.ranked, 1)",
            [ember_id.as_str()],
            |row| row.get(0),
        )
        .optional()?)
}

/// `account`'s rating, grown less sure by the time since its last game, and
/// the band last shown for it (0 for none yet).
fn load(tx: &Transaction<'_>, account: &str, now: u64) -> Result<(Rating, u8)> {
    let stored = tx
        .query_row(
            "SELECT rating, deviation, volatility, band, played_at
             FROM skill_ratings WHERE discord_user_id = ?1",
            [account],
            |row| {
                let rating = Rating {
                    rating: row.get(0)?,
                    deviation: row.get(1)?,
                    volatility: row.get(2)?,
                };
                Ok((rating, row.get(3)?, row.get::<_, u64>(4)?))
            },
        )
        .optional()?;
    Ok(match stored {
        Some((rating, band, played_at)) => (aged(rating, played_at, now), band),
        None => (Rating::NEW, 0),
    })
}

/// `rating` from a game at `played_at`, grown less sure by the days since.
fn aged(rating: Rating, played_at: u64, now: u64) -> Rating {
    rating.idle(now.saturating_sub(played_at) as f64 / SECS_PER_PERIOD as f64)
}

/// Writes `account`'s rating after a game at `now`, leaving the band shown.
fn store(tx: &Transaction<'_>, account: &str, rating: Rating, now: u64) -> Result<()> {
    tx.execute(
        "INSERT INTO skill_ratings
             (discord_user_id, rating, deviation, volatility, band, played_at)
         VALUES (?1, ?2, ?3, ?4, 0, ?5)
         ON CONFLICT (discord_user_id) DO UPDATE SET rating = ?2, deviation = ?3,
             volatility = ?4, played_at = ?5",
        params![
            account,
            rating.rating,
            rating.deviation,
            rating.volatility,
            now
        ],
    )?;
    Ok(())
}

/// The band to show for a settled `rating` when `shown` was shown last: its
/// own band the first time, then another only once the rating is past the
/// edge by the margin.
fn shown_band(shown: u8, rating: f64) -> u8 {
    match shown {
        0 => band_of(rating),
        band if band_of(rating - BAND_MARGIN) > band => band_of(rating - BAND_MARGIN),
        band if band_of(rating + BAND_MARGIN) < band => band_of(rating + BAND_MARGIN),
        band => band,
    }
}

/// The band `rating` falls in, 1 to `SKILL_BANDS`.
fn band_of(rating: f64) -> u8 {
    let floors_reached = BAND_FLOORS.iter().filter(|floor| rating >= **floor).count();
    1 + floors_reached as u8
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_start_is_the_floor_of_b_and_the_ends_are_open() {
        assert_eq!(band_of(1500.0), 5);
        assert_eq!(band_of(1499.9), 4);
        assert_eq!(band_of(1199.9), 1);
        assert_eq!(band_of(0.0), 1);
        assert_eq!(band_of(2049.9), 9);
        assert_eq!(band_of(2050.0), SKILL_BANDS);
        assert_eq!(band_of(9999.0), SKILL_BANDS);
    }

    #[test]
    fn a_new_player_shows_a_class_in_about_three_games_against_settled_peers() {
        let peer = Rating {
            rating: 1500.0,
            deviation: 60.0,
            volatility: 0.06,
        };
        let mut new = Rating::NEW;
        let mut games = 0;
        while new.deviation > SETTLED_DEVIATION {
            new = new.after(peer, f64::from(games % 2), 1.0);
            games += 1;
        }
        assert!((2..=4).contains(&games), "{games}");
        // Its games count in full for about ten more games.
        while new.deviation > UNSURE_DEVIATION {
            new = new.after(peer, f64::from(games % 2), 1.0);
            games += 1;
        }
        assert!((8..=14).contains(&games), "{games}");
        // An even record against B players lands next to B.
        assert!((4..=5).contains(&shown_band(0, new.rating)), "{new:?}");
    }

    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-9
    }

    fn settled(rating: f64) -> Rating {
        Rating {
            rating,
            deviation: 80.0,
            volatility: 0.06,
        }
    }

    #[test]
    fn a_lopsided_game_hardly_counts_whoever_wins() {
        assert!(close(gap_weight(settled(1500.0), settled(1650.0)), 1.0));
        assert!(close(gap_weight(settled(1500.0), settled(1800.0)), 0.55));
        assert!(close(
            gap_weight(settled(2100.0), settled(1650.0)),
            MIN_WEIGHT
        ));
        assert!(close(
            gap_weight(settled(2100.0), settled(1000.0)),
            MIN_WEIGHT
        ));
        // A Grand Master who loses to a D player gives up a few points, not a class.
        let (master, newer) = (settled(2100.0), settled(1150.0));
        let lost = master.rating - master.after(newer, 0.0, gap_weight(master, newer)).rating;
        let full = master.rating - master.after(newer, 0.0, 1.0).rating;
        assert!(lost < 10.0 && lost < full * 0.15, "{lost} of {full}");
        // A player still placing takes every game in full.
        assert_eq!(gap_weight(Rating::NEW, settled(2100.0)), 1.0);
    }

    #[test]
    fn an_upset_still_counts_for_the_winner() {
        let (master, newer) = (settled(2100.0), settled(1150.0));
        // The lower player beats the Grand Master: a fair climb for them, a
        // light loss for the master.
        let (winner, loser) = weights(newer, master);
        assert!(
            close(winner, UPSET_WEIGHT) && close(loser, MIN_WEIGHT),
            "{winner} {loser}"
        );
        let (climb, cost) = (
            newer.after(master, 1.0, UPSET_WEIGHT).rating - newer.rating,
            master.rating - master.after(newer, 0.0, MIN_WEIGHT).rating,
        );
        // About what beating a peer is worth, while the master gives up little.
        let peer_win = newer.after(settled(1150.0), 1.0, 1.0).rating - newer.rating;
        assert!(
            climb > peer_win * 0.9 && cost < 10.0,
            "{climb} {peer_win} {cost}"
        );
        // The expected result hardly counts for either.
        let (winner, loser) = weights(master, newer);
        assert!(
            close(winner, MIN_WEIGHT) && close(loser, MIN_WEIGHT),
            "{winner} {loser}"
        );
        // Between near peers an upset is an ordinary game.
        let (winner, loser) = weights(settled(1500.0), settled(1600.0));
        assert!(close(winner, 1.0) && close(loser, 1.0), "{winner} {loser}");
    }

    #[test]
    fn the_first_class_is_the_ratings_own() {
        assert_eq!(shown_band(0, 1601.0), 6);
        assert_eq!(shown_band(0, 1599.0), 5);
    }

    #[test]
    fn the_class_moves_only_past_the_margin() {
        // B shown; B+ starts at 1600.
        assert_eq!(shown_band(5, 1610.0), 5);
        assert_eq!(shown_band(5, 1625.0), 6);
        // B+ shown, back below 1600.
        assert_eq!(shown_band(6, 1590.0), 6);
        assert_eq!(shown_band(6, 1574.0), 5);
        // A big move lands where the margin allows, not one class at a time.
        assert_eq!(shown_band(5, 1900.0), 8);
    }
}
