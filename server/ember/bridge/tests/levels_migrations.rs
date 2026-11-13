//! The migrations of the levels: 060 keeps every rated game, sitting and
//! rating it finds from a database at 056, and leaves behind a rated game
//! whose room is gone; 062 adds the account points and keeps the fighter
//! points; 063 adds the award counters and starts them from what is kept; 064 adds
//! the player cards and starts them from what friends and rankings know; 065
//! adds the wins without a class bonus and keeps everybody's points.
//! Each starts from a database as a bridge before it left it.
use std::path::Path;

use ember_bridge::{Db, testing::TempDir};
use rusqlite::OptionalExtension;

/// Applies `migrations/` up to 056 as a bridge from before 060 did, each
/// recorded by its number only.
fn database_at_056(path: &Path) -> rusqlite::Connection {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("migrations");
    let mut names: Vec<String> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|entry| entry.unwrap().file_name().into_string().unwrap())
        .filter(|name| name.as_str() < "057")
        .collect();
    names.sort();
    assert_eq!(names.len(), 56);
    let mut connection = rusqlite::Connection::open(path).unwrap();
    connection
        .execute_batch(
            "CREATE TABLE schema_migrations (version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)",
        )
        .unwrap();
    for name in names {
        let version: i64 = name[..3].parse().unwrap();
        let transaction = connection.transaction().unwrap();
        transaction
            .execute_batch(&std::fs::read_to_string(dir.join(&name)).unwrap())
            .unwrap();
        transaction
            .execute(
                "INSERT INTO schema_migrations (version, applied_at) VALUES (?1, 1000)",
                [version],
            )
            .unwrap();
        transaction.commit().unwrap();
    }
    connection
}

/// What the bridge holds of the rating: games rated, sittings and ratings.
fn rating_state(connection: &rusqlite::Connection) -> (Vec<String>, Vec<String>, Vec<String>) {
    let all = |query: &str| -> Vec<String> {
        connection
            .prepare(query)
            .unwrap()
            .query_map([], |row| row.get(0))
            .unwrap()
            .collect::<rusqlite::Result<_>>()
            .unwrap()
    };
    (
        all("SELECT room_id || ':' || table_id || ':' || generation FROM rated_games ORDER BY 1"),
        all(
            "SELECT first || ':' || second || ':' || games || ':' || played_at FROM rated_pairs ORDER BY 1",
        ),
        all(
            "SELECT discord_user_id || ':' || rating || ':' || deviation || ':' || band FROM skill_ratings ORDER BY 1",
        ),
    )
}

#[tokio::test]
async fn migration_060_keeps_rated_games_sittings_and_ratings_and_an_older_bridge_still_works() {
    let dir = TempDir::new("levels-migration");
    let path = dir.join("bridge.sqlite3");
    let before = {
        let connection = database_at_056(&path);
        connection
            .execute_batch(
                "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at)
                   VALUES ('room', 'Friendly matches', 8, 'build', 'emb_player1', 1000);
                 INSERT INTO rated_games (room_id, table_id, generation)
                   VALUES ('room', 0, 1), ('room', 0, 2), ('room', 7, 9223372036854775807);
                 INSERT INTO rated_pairs (first, second, games, played_at)
                   VALUES ('100', '200', 3, 5000), ('100', '300', 9, 6000);
                 INSERT INTO skill_ratings (discord_user_id, rating, deviation, volatility, band, played_at)
                   VALUES ('100', 1612.5, 88.25, 0.06, 6, 5000), ('200', 1387.5, 91.0, 0.06, 4, 5000);",
            )
            .unwrap();
        rating_state(&connection)
    };
    drop(Db::open(&path).unwrap());

    let connection = rusqlite::Connection::open(&path).unwrap();
    assert_eq!(rating_state(&connection), before);
    // Every game rated before is its battle's first.
    let games: Vec<i64> = connection
        .prepare("SELECT game FROM rated_games")
        .unwrap()
        .query_map([], |row| row.get(0))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    assert_eq!(games, vec![0, 0, 0]);
    for table in ["fighter_levels", "pair_records"] {
        let rows: i64 = connection
            .query_row(&format!("SELECT COUNT(*) FROM {table}"), [], |row| {
                row.get(0)
            })
            .unwrap();
        assert_eq!(rows, 0, "{table}");
    }
    // 060 joins the key: the next game of a battle is a game of its own.
    let inserted = |table: i64, generation: i64, game: i64| {
        connection
            .execute(
                "INSERT OR IGNORE INTO rated_games (room_id, table_id, generation, game)
                 VALUES ('room', ?1, ?2, ?3)",
                [table, generation, game],
            )
            .unwrap()
    };
    assert_eq!(inserted(0, 1, 0), 0);
    assert_eq!(inserted(0, 1, 1), 1);

    // A bridge rolled back to before 060 (the shared library's, which runs
    // 057 to 059 and names the game in its keys) writes what it always wrote:
    // its key is a battle's first game, and the rated games stay rated.
    let older = |table: i64, generation: i64| {
        connection
            .execute(
                "INSERT OR IGNORE INTO rated_games (room_id, table_id, generation)
                 SELECT room_id, ?2, ?3 FROM rooms WHERE room_id = ?1",
                rusqlite::params!["room", table, generation],
            )
            .unwrap()
    };
    assert_eq!(older(0, 2), 0);
    assert_eq!(older(0, 4), 1);
    connection
        .execute_batch(
            "DELETE FROM rated_pairs WHERE played_at < 5500;
             INSERT INTO rated_pairs (first, second, games, played_at) VALUES ('100', '300', 1, 7000)
               ON CONFLICT (game, first, second) DO UPDATE SET games = games + 1, played_at = 7000;",
        )
        .unwrap();
    drop(connection);

    // Opened again by this bridge, nothing runs twice.
    drop(Db::open(&path).unwrap());
    let connection = rusqlite::Connection::open(&path).unwrap();
    let (rated, pairs, ratings) = rating_state(&connection);
    assert_eq!(rated.len(), 5);
    assert_eq!(pairs, vec!["100:300:10:7000".to_owned()]);
    assert_eq!(ratings, before.2);
}

/// A bridge can hold a rated game whose room was deleted while foreign keys
/// were off. The rebuilt table checks its key as each row lands, so the copy
/// leaves that game behind rather than stopping the bridge from starting.
#[tokio::test]
async fn migration_060_leaves_behind_a_rated_game_whose_room_is_gone_and_keeps_the_rest() {
    let dir = TempDir::new("levels-migration-orphan");
    let path = dir.join("bridge.sqlite3");
    {
        let connection = database_at_056(&path);
        // Foreign keys are off on this connection, as they were for the bridge
        // that lost the room: the delete does not cascade. SQLite here starts
        // with them on, so they are turned off by name.
        connection
            .execute_batch(
                "PRAGMA foreign_keys = OFF;
                 INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at)
                   VALUES ('room', 'Friendly matches', 8, 'build', 'emb_player1', 1000),
                          ('gone', 'Player 1''s room', 8, 'build', 'emb_player1', 1000);
                 INSERT INTO rated_games (room_id, table_id, generation)
                   VALUES ('room', 0, 1), ('gone', 0, 1), ('room', 7, 2), ('gone', 3, 5);
                 DELETE FROM rooms WHERE room_id = 'gone';",
            )
            .unwrap();
        let orphans: i64 = connection
            .query_row(
                "SELECT COUNT(*) FROM rated_games WHERE room_id = 'gone'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(orphans, 2, "the fixture must hold rows without a room");
    }
    drop(Db::open(&path).unwrap());

    let connection = rusqlite::Connection::open(&path).unwrap();
    let kept: Vec<(String, i64, i64, i64)> = connection
        .prepare("SELECT room_id, table_id, generation, game FROM rated_games ORDER BY table_id")
        .unwrap()
        .query_map([], |row| {
            Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?))
        })
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    assert_eq!(
        kept,
        vec![("room".to_owned(), 0, 1, 0), ("room".to_owned(), 7, 2, 0)]
    );
}

/// A database as a bridge from before 062 left it: this bridge's own ledger
/// (060 and 061 are recorded by name, as only this line writes them), without
/// the account points, and without what 065 adds to them and to the fighter
/// points.
fn database_at_061(path: &Path) -> rusqlite::Connection {
    drop(Db::open(path).unwrap());
    let connection = rusqlite::Connection::open(path).unwrap();
    connection
        .execute_batch(
            "DROP TABLE account_points;
             ALTER TABLE fighter_levels DROP COLUMN lower_wins;
             DELETE FROM schema_migrations WHERE version IN (62, 65);",
        )
        .unwrap();
    connection
}

#[tokio::test]
async fn migration_062_adds_account_points_and_keeps_the_fighter_points() {
    let dir = TempDir::new("account-points-migration");
    let path = dir.join("bridge.sqlite3");
    {
        let connection = database_at_061(&path);
        connection
            .execute_batch(
                "INSERT INTO fighter_levels (discord_user_id, fighter, games, wins, first_at, played_at)
                   VALUES ('100', 3, 4, 2, 1000, 2000);",
            )
            .unwrap();
    }
    drop(Db::open(&path).unwrap());

    let connection = rusqlite::Connection::open(&path).unwrap();
    let rows: i64 = connection
        .query_row("SELECT COUNT(*) FROM account_points", [], |row| row.get(0))
        .unwrap();
    assert_eq!(rows, 0);
    let kept: (i64, i64) = connection
        .query_row(
            "SELECT games, wins FROM fighter_levels WHERE discord_user_id = '100'",
            [],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .unwrap();
    assert_eq!(kept, (4, 2));
    // One row an account, and never more wins than games.
    connection
        .execute(
            "INSERT INTO account_points (discord_user_id, games, wins, first_at, played_at)
             VALUES ('100', 4, 2, 1000, 2000)",
            [],
        )
        .unwrap();
    for statement in [
        "INSERT INTO account_points (discord_user_id, games, wins, first_at, played_at)
         VALUES ('100', 1, 0, 1, 1)",
        "INSERT INTO account_points (discord_user_id, games, wins, first_at, played_at)
         VALUES ('200', 1, 2, 1, 1)",
    ] {
        assert!(connection.execute(statement, []).is_err(), "{statement}");
    }
    drop(connection);

    // Opened again by this bridge, nothing runs twice.
    drop(Db::open(&path).unwrap());
    let connection = rusqlite::Connection::open(&path).unwrap();
    let rows: i64 = connection
        .query_row("SELECT COUNT(*) FROM account_points", [], |row| row.get(0))
        .unwrap();
    assert_eq!(rows, 1);
}

/// A database as a bridge from before 063 left it: this bridge's own ledger
/// without the award counters.
fn database_at_062(path: &Path) -> rusqlite::Connection {
    drop(Db::open(path).unwrap());
    let connection = rusqlite::Connection::open(path).unwrap();
    connection
        .execute_batch(
            "DROP TABLE player_cards;
             DROP TABLE award_counters;
             DROP INDEX IF EXISTS pair_records_second;
             DELETE FROM schema_migrations WHERE version IN (63, 64);",
        )
        .unwrap();
    connection
}

#[tokio::test]
async fn migration_063_starts_the_award_counters_from_what_is_kept() {
    let dir = TempDir::new("award-counters-migration");
    let path = dir.join("bridge.sqlite3");
    {
        let connection = database_at_062(&path);
        connection
            .execute_batch(
                "INSERT INTO account_points (discord_user_id, games, wins, first_at, played_at)
                   VALUES ('100', 4, 2, 1000, 2000), ('200', 3, 0, 1000, 2000);
                 INSERT INTO skill_ratings
                   (game, discord_user_id, rating, deviation, volatility, band, played_at)
                   VALUES ('usf4', '100', 1650, 80, 0.06, 6, 2000),
                          ('usf4', '300', 1500, 80, 0.06, 5, 2000),
                          ('usf4', '400', 1500, 80, 0.06, 0, 2000);",
            )
            .unwrap();
    }
    drop(Db::open(&path).unwrap());

    let connection = rusqlite::Connection::open(&path).unwrap();
    let counters: Vec<(String, i64, i64, i64)> = connection
        .prepare(
            "SELECT discord_user_id, streak, best_streak, peak_band FROM award_counters
             ORDER BY discord_user_id",
        )
        .unwrap()
        .query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    // A win is a run of one and a class is the peak so far; an account that
    // never won starts at 0, one with a class but no game keeps the class,
    // and one still placing has no row.
    assert_eq!(
        counters,
        vec![
            ("100".to_owned(), 0, 1, 6),
            ("200".to_owned(), 0, 0, 0),
            ("300".to_owned(), 0, 0, 5),
        ]
    );
    // A run never passes the best one, and a peak is a class.
    for statement in [
        "INSERT INTO award_counters VALUES ('500', 2, 1, 0)",
        "INSERT INTO award_counters VALUES ('500', 0, 0, 11)",
    ] {
        assert!(connection.execute(statement, []).is_err(), "{statement}");
    }
    drop(connection);

    // Opened again by this bridge, nothing runs twice.
    drop(Db::open(&path).unwrap());
    let connection = rusqlite::Connection::open(&path).unwrap();
    let rows: i64 = connection
        .query_row("SELECT COUNT(*) FROM award_counters", [], |row| row.get(0))
        .unwrap();
    assert_eq!(rows, 3);
}

/// A database as a bridge from before 064 left it: this bridge's own ledger
/// without the player cards.
fn database_at_063(path: &Path) -> rusqlite::Connection {
    drop(Db::open(path).unwrap());
    let connection = rusqlite::Connection::open(path).unwrap();
    connection
        .execute_batch("DROP TABLE player_cards; DELETE FROM schema_migrations WHERE version = 64;")
        .unwrap();
    connection
}

#[tokio::test]
async fn migration_064_starts_the_player_cards_from_what_friends_and_rankings_know() {
    let dir = TempDir::new("player-cards-migration");
    let path = dir.join("bridge.sqlite3");
    {
        let connection = database_at_063(&path);
        connection
            .execute_batch(
                "INSERT INTO social_accounts
                   (discord_user_id, name, country, main_fighter, code, code_at, seen_at, off)
                   VALUES ('100', 'Player 1', 'DE', 3, 'AAAA-0001', 1, 5000, 0),
                          ('200', 'Player 2', NULL, NULL, 'AAAA-0002', 1, 6000, 1);
                 INSERT INTO identities (ember_id, public_key, created_at) VALUES ('emb_one', 'one-key', 1);
                 INSERT INTO discord_accounts (user_id, ember_id, username, connected_at)
                   VALUES ('100', 'emb_one', 'discord_one', 1500);
                 INSERT INTO ranking_choices (game, discord_user_id, ranked, changed_at, name, country)
                   VALUES ('usf4', '200', 1, 7000, 'Ranked Two', 'FR'),
                          ('usf4', '300', 1, 7000, 'Player 3', 'JP'),
                          ('usf4', '400', 0, NULL, NULL, NULL);",
            )
            .unwrap();
    }
    drop(Db::open(&path).unwrap());

    let connection = rusqlite::Connection::open(&path).unwrap();
    type Card = (String, String, Option<String>, Option<i64>, i64, i64);
    let cards: Vec<Card> = connection
        .prepare(
            "SELECT discord_user_id, name, country, main_fighter, since, updated_at
             FROM player_cards ORDER BY discord_user_id",
        )
        .unwrap()
        .query_map([], |row| {
            Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?, row.get(4)?, row.get(5)?))
        })
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    // An account friends know keeps what it sent last, from its Discord
    // connection when it has one and else from when it was last seen; one only
    // rankings know has no main and came now; one with no name has no card.
    assert_eq!(cards.len(), 3, "{cards:?}");
    assert_eq!(
        cards[0],
        ("100".into(), "Player 1".into(), Some("DE".into()), Some(3), 1500, 5000)
    );
    assert_eq!(cards[1], ("200".into(), "Player 2".into(), None, None, 6000, 6000));
    assert_eq!((&cards[2].0[..], &cards[2].1[..], cards[2].2.as_deref()), ("300", "Player 3", Some("JP")));
    assert_eq!((cards[2].3, cards[2].5), (None, 7000));
    assert!(cards[2].4 > 1_000_000_000, "{}", cards[2].4);

    // A name is found in any case, and the small numbers stay in range.
    let found: String = connection
        .query_row(
            "SELECT discord_user_id FROM player_cards WHERE name = 'player 1'",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(found, "100");
    for statement in [
        "UPDATE player_cards SET device = 5 WHERE discord_user_id = '100'",
        "UPDATE player_cards SET link = 3 WHERE discord_user_id = '100'",
        "UPDATE player_cards SET looking = 64 WHERE discord_user_id = '100'",
        "UPDATE player_cards SET main_fighter = 44 WHERE discord_user_id = '100'",
        "INSERT INTO player_cards (discord_user_id, name, since, updated_at) VALUES ('100', 'Again', 1, 1)",
    ] {
        assert!(connection.execute(statement, []).is_err(), "{statement}");
    }
    drop(connection);

    // Opened again by this bridge, nothing runs twice.
    drop(Db::open(&path).unwrap());
    let connection = rusqlite::Connection::open(&path).unwrap();
    let rows: i64 = connection
        .query_row("SELECT COUNT(*) FROM player_cards", [], |row| row.get(0))
        .unwrap();
    assert_eq!(rows, 3);
}

/// A database as a bridge from before 065 left it: this bridge's own ledger
/// without the wins that earned no class bonus.
fn database_at_064(path: &Path) -> rusqlite::Connection {
    drop(Db::open(path).unwrap());
    let connection = rusqlite::Connection::open(path).unwrap();
    connection
        .execute_batch(
            "ALTER TABLE fighter_levels DROP COLUMN lower_wins;
             ALTER TABLE account_points DROP COLUMN lower_wins;
             DELETE FROM schema_migrations WHERE version = 65;",
        )
        .unwrap();
    connection
}

/// Every count of points as `points` works them out: the accounts' rows, then
/// the fighters', each as `account:fighter:games:wins:points`.
fn points_state(connection: &rusqlite::Connection, points: &str) -> Vec<String> {
    connection
        .prepare(&format!(
            "SELECT discord_user_id || '::' || games || ':' || wins || ':' || ({points})
             FROM account_points
             UNION ALL
             SELECT discord_user_id || ':' || fighter || ':' || games || ':' || wins || ':' || ({points})
             FROM fighter_levels ORDER BY 1"
        ))
        .unwrap()
        .query_map([], |row| row.get(0))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap()
}

#[tokio::test]
async fn migration_065_keeps_every_accounts_points_and_an_older_bridge_still_counts() {
    let dir = TempDir::new("class-bonus-migration");
    let path = dir.join("bridge.sqlite3");
    // What a bridge from before 065 served: games plus wins.
    let before = {
        let connection = database_at_064(&path);
        connection
            .execute_batch(
                "INSERT INTO account_points (discord_user_id, games, wins, first_at, played_at)
                   VALUES ('100', 40, 25, 1000, 2000), ('200', 7, 0, 1000, 2000),
                          ('300', 3, 3, 1000, 2000);
                 INSERT INTO fighter_levels (discord_user_id, fighter, games, wins, first_at, played_at)
                   VALUES ('100', 3, 30, 20, 1000, 2000), ('100', 7, 10, 5, 1000, 2000),
                          ('200', 12, 7, 0, 1000, 2000), ('300', 43, 3, 3, 1000, 2000);",
            )
            .unwrap();
        points_state(&connection, "games + wins")
    };
    assert_eq!(before.len(), 7);
    assert!(before.contains(&"100::40:25:65".to_owned()), "{before:?}");
    drop(Db::open(&path).unwrap());

    // The same games, wins and points for every account and fighter: no win
    // so far loses its bonus.
    let connection = rusqlite::Connection::open(&path).unwrap();
    assert_eq!(points_state(&connection, "games + wins - lower_wins"), before);
    assert_eq!(points_state(&connection, "games + wins"), before);

    // A bridge from before 065, put back, counts a won game as it always
    // did: an old row and a new one, and neither win is without its bonus.
    connection
        .execute_batch(
            "INSERT INTO fighter_levels (discord_user_id, fighter, games, wins, first_at, played_at)
               VALUES ('100', 3, 1, 1, 3000, 3000)
               ON CONFLICT (discord_user_id, fighter) DO UPDATE SET games = games + 1,
                   wins = wins + 1, played_at = 3000;
             INSERT INTO account_points (discord_user_id, games, wins, first_at, played_at)
               VALUES ('400', 1, 1, 3000, 3000)
               ON CONFLICT (discord_user_id) DO UPDATE SET games = games + 1,
                   wins = wins + 1, played_at = 3000;",
        )
        .unwrap();
    let after = points_state(&connection, "games + wins - lower_wins");
    assert!(after.contains(&"100:3:31:21:52".to_owned()), "{after:?}");
    assert!(after.contains(&"400::1:1:2".to_owned()), "{after:?}");

    // The wins without a bonus are some of the wins, never more and never
    // fewer than none.
    connection
        .execute(
            "UPDATE account_points SET lower_wins = 25 WHERE discord_user_id = '100'",
            [],
        )
        .unwrap();
    for statement in [
        "UPDATE account_points SET lower_wins = 26 WHERE discord_user_id = '100'",
        "UPDATE account_points SET lower_wins = -1 WHERE discord_user_id = '100'",
        "UPDATE fighter_levels SET lower_wins = 1 WHERE discord_user_id = '200'",
        "UPDATE fighter_levels SET lower_wins = -1 WHERE discord_user_id = '200'",
    ] {
        assert!(connection.execute(statement, []).is_err(), "{statement}");
    }
    drop(connection);

    // Opened again by this bridge, nothing runs twice: what was counted since stays.
    drop(Db::open(&path).unwrap());
    let connection = rusqlite::Connection::open(&path).unwrap();
    let kept: i64 = connection
        .query_row(
            "SELECT lower_wins FROM account_points WHERE discord_user_id = '100'",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(kept, 25);
}
