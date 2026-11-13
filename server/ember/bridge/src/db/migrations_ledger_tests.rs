//! The ledger of what ran: the records of a bridge from before file names, the two
//! files 055 has meant, the library's 057 to 059 adopted only when the schema shows
//! them, and the refusal of another game's rows.
use rusqlite::Connection;

use super::tests::has_table;
use super::*;

/// What a bridge from before file names did with `migrations`: each applied
/// in its own transaction and recorded by number only, at time 1000.
fn as_an_older_bridge(connection: &mut Connection, migrations: &[(i64, &str)]) {
    connection
        .execute_batch(
            "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)",
        )
        .unwrap();
    for (version, sql) in migrations {
        let transaction = connection.transaction().unwrap();
        transaction.execute_batch(sql).unwrap();
        transaction
            .execute(
                "INSERT INTO schema_migrations (version, applied_at) VALUES (?1, 1000)",
                [version],
            )
            .unwrap();
        transaction.commit().unwrap();
    }
}

/// This bridge's files up to `last`, as an older bridge listed them.
fn files_to(last: i64) -> Vec<(i64, &'static str)> {
    MIGRATIONS
        .iter()
        .take_while(|migration| migration.version <= last)
        .map(|migration| (migration.version, migration.sql))
        .collect()
}

fn file(version: i64) -> Migration {
    *MIGRATIONS
        .iter()
        .find(|migration| migration.version == version)
        .unwrap()
}

/// `schema_migrations` as (version, name, checksum).
fn records(connection: &Connection) -> Vec<(i64, Option<String>, Option<String>)> {
    connection
        .prepare("SELECT version, name, checksum FROM schema_migrations ORDER BY version")
        .unwrap()
        .query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap()
}

fn applied_at(connection: &Connection, version: i64) -> i64 {
    connection
        .query_row(
            "SELECT applied_at FROM schema_migrations WHERE version = ?1",
            [version],
            |row| row.get(0),
        )
        .unwrap()
}

/// Every file of this bridge is recorded by its number, name and checksum,
/// and nothing else is.
fn assert_every_file_recorded(connection: &Connection) {
    let expected: Vec<_> = MIGRATIONS
        .iter()
        .map(|migration| {
            (
                migration.version,
                Some(migration.name.to_owned()),
                Some(migration.checksum()),
            )
        })
        .collect();
    assert_eq!(records(connection), expected);
}

fn refusal(result: Result<(), OpenError>) -> String {
    match result {
        Err(OpenError::Migration(text)) => text,
        other => panic!("expected a refusal, got {other:?}"),
    }
}

fn wired_room(connection: &Connection) {
    connection
        .execute(
            "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at, wired_only)
             VALUES ('wired', 'Wired room', 8, 'build', 'emb_player1', 1000, 1)",
            [],
        )
        .unwrap();
}

fn wired_only(connection: &Connection) -> i64 {
    connection
        .query_row(
            "SELECT wired_only FROM rooms WHERE room_id = 'wired'",
            [],
            |row| row.get(0),
        )
        .unwrap()
}

#[test]
fn every_file_under_migrations_is_listed_once_in_order() {
    let mut files: Vec<String> =
        std::fs::read_dir(concat!(env!("CARGO_MANIFEST_DIR"), "/migrations"))
            .unwrap()
            .map(|entry| entry.unwrap().file_name().into_string().unwrap())
            .collect();
    files.sort();
    let listed: Vec<&str> = MIGRATIONS.iter().map(|migration| migration.name).collect();
    assert_eq!(files, listed);
    assert!(
        MIGRATIONS
            .windows(2)
            .all(|pair| pair[0].version < pair[1].version)
    );
}

#[test]
fn a_checksum_is_the_same_for_a_windows_checkout() {
    let unix = Migration {
        version: 1,
        name: "001_example.sql",
        sql: "SELECT 1;\nSELECT 2;\n",
    };
    let windows = Migration {
        sql: "SELECT 1;\r\nSELECT 2;\r\n",
        ..unix
    };
    assert_eq!(unix.checksum(), windows.checksum());
    let changed = Migration {
        sql: "SELECT 1;\n",
        ..unix
    };
    assert_ne!(unix.checksum(), changed.checksum());
}

#[test]
fn a_new_database_records_each_file_by_name_and_checksum() {
    let mut connection = Connection::open_in_memory().unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    let first = applied_at(&connection, 56);
    // Opening it again changes nothing.
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    assert_eq!(applied_at(&connection, 56), first);
}

/// Production: an older bridge ran 001 to 054, the archive branch's 055,
/// then release's 056, recording numbers only.
#[test]
fn records_by_number_only_take_this_bridges_names_and_keep_when_they_ran() {
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(56));
    // An older bridge's record has no name or checksum columns at all.
    let named: bool = connection
        .query_row(
            "SELECT EXISTS (SELECT 1 FROM pragma_table_info('schema_migrations') WHERE name = 'name')",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert!(!named);
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    assert_eq!(applied_at(&connection, 55), 1000);
    assert_eq!(applied_at(&connection, 56), 1000);
}

#[test]
fn a_database_an_older_bridge_left_at_054_runs_055_and_056() {
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(54));
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    assert_eq!(applied_at(&connection, 54), 1000);
    assert_ne!(applied_at(&connection, 55), 1000);
    assert!(has_table(&connection, "archive_keys"));
    connection.prepare("SELECT wired_only FROM rooms").unwrap();
}

/// Staging and the Nightly bridge: release and nightly listed 056 and left
/// 055 out, so the archive listing index never ran there.
#[test]
fn a_database_at_056_without_055_runs_the_archive_listing_index() {
    let mut connection = Connection::open_in_memory().unwrap();
    let without_055: Vec<_> = files_to(56)
        .into_iter()
        .filter(|(version, _)| *version != 55)
        .collect();
    as_an_older_bridge(&mut connection, &without_055);
    wired_room(&connection);
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    assert_ne!(applied_at(&connection, 55), 1000);
    assert_eq!(applied_at(&connection, 56), 1000);
    assert!(has_table(&connection, "archive_keys"));
    assert_eq!(wired_only(&connection), 1);
}

/// The wired-only rule was first committed as 055 (35494c167), the same
/// file 056 is now. A bridge built from it recorded the column as 55; the
/// bridge after it would then skip the index and fail 056 on the column.
#[test]
fn a_database_whose_055_was_the_wired_only_column_moves_it_to_056_and_runs_the_index() {
    let mut connection = Connection::open_in_memory().unwrap();
    let mut old = files_to(54);
    old.push((55, file(56).sql));
    as_an_older_bridge(&mut connection, &old);
    wired_room(&connection);
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    // 56 is when the column came; 55 ran just now.
    assert_eq!(applied_at(&connection, 56), 1000);
    assert_ne!(applied_at(&connection, 55), 1000);
    assert!(has_table(&connection, "archive_keys"));
    assert_eq!(wired_only(&connection), 1);
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
}

#[test]
fn an_unnamed_055_that_is_neither_file_is_refused() {
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(54));
    connection
        .execute(
            "INSERT INTO schema_migrations (version, applied_at) VALUES (55, 1000)",
            [],
        )
        .unwrap();
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("055"), "{text}");
    // Nothing changed: the record has its two columns still, and the index
    // did not run.
    let columns: i64 = connection
        .query_row(
            "SELECT COUNT(*) FROM pragma_table_info('schema_migrations')",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(columns, 2);
    assert!(!has_table(&connection, "archive_keys"));
}

#[test]
fn a_changed_file_or_another_file_under_a_number_is_refused_by_name() {
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(56));
    migrate(&mut connection, MIGRATIONS).unwrap();
    let tamper = |connection: &Connection, set: &str| {
        connection
            .execute(
                &format!("UPDATE schema_migrations SET {set} WHERE version = 30"),
                [],
            )
            .unwrap();
    };
    tamper(&connection, "checksum = '00'");
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("030_ranking_choices.sql"), "{text}");
    tamper(
        &connection,
        &format!(
            "checksum = '{}', name = '030_elsewhere.sql'",
            file(30).checksum()
        ),
    );
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("030_elsewhere.sql"), "{text}");
    assert!(text.contains("030_ranking_choices.sql"), "{text}");
    tamper(&connection, "name = '030_ranking_choices.sql'");
    migrate(&mut connection, MIGRATIONS).unwrap();
}

#[test]
fn a_number_this_bridge_lacks_is_refused_below_its_newest_and_left_alone_above() {
    let mut connection = Connection::open_in_memory().unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    // A bridge from a line without 030 opens it.
    let without_030: Vec<_> = MIGRATIONS
        .iter()
        .filter(|migration| migration.version != 30)
        .copied()
        .collect();
    let text = refusal(migrate(&mut connection, &without_030));
    assert!(text.contains("030"), "{text}");
    // A bridge rolled back to before 055 leaves a newer bridge's 055 and 056.
    migrate(&mut connection, &MIGRATIONS[..54]).unwrap();
    assert_every_file_recorded(&connection);
}

#[test]
fn an_unnamed_record_is_adopted_up_to_059_and_refused_above() {
    // A bridge from before the ledger, at 056.
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(56));
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);

    // Production and staging: the shared library's bridge ran its 057 to 059,
    // which this bridge carries byte for byte, and recorded numbers only.
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(59));
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    assert!(has_table(&connection, "fighter_levels"));

    // An unnamed 060 is no file this bridge knows: its own are named.
    let mut connection = Connection::open_in_memory().unwrap();
    let mut older = files_to(59);
    older.push((60, "SELECT 1;"));
    as_an_older_bridge(&mut connection, &older);
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("060"), "{text}");
    assert!(text.contains("another line"), "{text}");
    // Nothing of this bridge's was applied on top of it.
    assert!(!has_table(&connection, "fighter_levels"));
}

#[test]
fn a_new_database_runs_the_librarys_files_then_the_1_6_files() {
    let mut connection = Connection::open_in_memory().unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    let versions: Vec<i64> = records(&connection).iter().map(|r| r.0).collect();
    assert_eq!(&versions[55..], &[56, 57, 58, 59, 60, 61, 62, 63, 64]);
    assert!(has_table(&connection, "fighter_levels"));
    assert!(has_table(&connection, "social_accounts"));
    assert!(has_table(&connection, "account_points"));
    assert!(has_table(&connection, "award_counters"));
    assert!(has_table(&connection, "player_cards"));
}

#[test]
fn an_unnamed_056_is_adopted_only_when_the_schema_shows_it_ran() {
    // 056 recorded, its column never added.
    let mut connection = Connection::open_in_memory().unwrap();
    let mut older = files_to(55);
    older.push((56, "SELECT 1;"));
    as_an_older_bridge(&mut connection, &older);
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("056") && text.contains("rooms.wired_only"), "{text}");
    assert!(!text.contains("  "), "{text}");
    assert!(!has_table(&connection, "fighter_levels"));
}

#[test]
fn an_unnamed_057_to_059_is_adopted_only_when_the_schema_shows_it_ran() {
    // 057 recorded, its column never added.
    let mut connection = Connection::open_in_memory().unwrap();
    let mut older = files_to(56);
    older.push((57, "SELECT 1;"));
    as_an_older_bridge(&mut connection, &older);
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("057"), "{text}");
    assert!(text.contains("rooms.game"), "{text}");
    assert!(!has_table(&connection, "fighter_levels"));

    // 058 recorded over the old keys of the rating tables.
    let mut connection = Connection::open_in_memory().unwrap();
    let mut older = files_to(57);
    older.push((58, "SELECT 1;"));
    as_an_older_bridge(&mut connection, &older);
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("058") && text.contains("skill_ratings"), "{text}");

    // 059 recorded without the column on matches.
    let mut connection = Connection::open_in_memory().unwrap();
    let mut older = files_to(58);
    older.push((59, "SELECT 1;"));
    as_an_older_bridge(&mut connection, &older);
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("059") && text.contains("matches.game"), "{text}");

    // All three with their schema are adopted.
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(59));
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
}

#[test]
fn a_database_holding_another_games_rows_is_refused_naming_the_tables() {
    let mut connection = Connection::open_in_memory().unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    connection
        .execute_batch(
            "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at)
               VALUES ('room', 'Friendly matches', 8, 'build', 'emb_player1', 1000);
             INSERT INTO skill_ratings (game, discord_user_id, rating, deviation, volatility, band, played_at)
               VALUES ('usf4', '100', 1500, 90, 0.06, 5, 1);",
        )
        .unwrap();
    // Only USF4's rows: it opens, again and again.
    migrate(&mut connection, MIGRATIONS).unwrap();

    connection
        .execute_batch("UPDATE rooms SET game = 'sfv';")
        .unwrap();
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("rooms"), "{text}");
    assert!(!text.contains("skill_ratings"), "{text}");
    assert!(text.contains("clear-sfv-rows.py"), "{text}");
    assert!(!text.contains("  "), "{text}");

    connection
        .execute_batch(
            "INSERT INTO skill_ratings (game, discord_user_id, rating, deviation, volatility, band, played_at)
               VALUES ('sfv', '100', 1500, 90, 0.06, 5, 1);
             INSERT INTO ranking_choices (game, discord_user_id, ranked) VALUES ('sfv', '100', 1);",
        )
        .unwrap();
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("rooms, skill_ratings, ranking_choices"), "{text}");
}

#[test]
fn an_older_database_with_no_game_columns_is_not_refused_for_them() {
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(56));
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
}
