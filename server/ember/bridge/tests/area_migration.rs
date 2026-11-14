//! Migration 066: rooms gain their creator's area, every room the database
//! already holds has none, and a card's region that a Nightly before areas
//! saved as its relay's code reads as no region. A bridge from before 066,
//! put back, still opens rooms. It starts from a database as such a bridge
//! left it.
use std::path::Path;

use ember_bridge::{Db, testing::TempDir};

/// A database as a bridge from before 066 left it: this bridge's own ledger
/// without the room's area.
fn database_at_065(path: &Path) -> rusqlite::Connection {
    drop(Db::open(path).unwrap());
    let connection = rusqlite::Connection::open(path).unwrap();
    connection
        .execute_batch(
            "ALTER TABLE rooms DROP COLUMN creator_area;
             DELETE FROM schema_migrations WHERE version = 66;",
        )
        .unwrap();
    connection
}

fn all(connection: &rusqlite::Connection, query: &str) -> Vec<String> {
    connection
        .prepare(query)
        .unwrap()
        .query_map([], |row| row.get(0))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap()
}

#[tokio::test]
async fn migration_066_adds_the_rooms_area_and_clears_a_cards_relay_code() {
    let dir = TempDir::new("area-migration");
    let path = dir.join("bridge.sqlite3");
    {
        let connection = database_at_065(&path);
        connection
            .execute_batch(
                "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at, creator_region)
                   VALUES ('room', 'Friendly matches', 8, 'build', 'emb_player1', 1000, 'use1');
                 INSERT INTO player_cards (discord_user_id, name, since, region, updated_at)
                   VALUES ('100', 'Player 1', 1, 'use1', 1), ('200', 'Player 2', 1, 'euc1', 1),
                          ('300', 'Player 3', 1, NULL, 1), ('400', 'Player 4', 1, 'aps1', 1);",
            )
            .unwrap();
    }
    drop(Db::open(&path).unwrap());

    let connection = rusqlite::Connection::open(&path).unwrap();
    // The room keeps its creator's relay and has no area.
    assert_eq!(
        all(
            &connection,
            "SELECT room_id || ':' || creator_region || ':' || COALESCE(creator_area, '-') FROM rooms"
        ),
        ["room:use1:-"]
    );
    // No card keeps a relay's code as its region; everything else stays.
    assert_eq!(
        all(
            &connection,
            "SELECT discord_user_id || ':' || name || ':' || COALESCE(region, '-')
             FROM player_cards ORDER BY discord_user_id"
        ),
        [
            "100:Player 1:-",
            "200:Player 2:-",
            "300:Player 3:-",
            "400:Player 4:-"
        ]
    );
    // A card saved since keeps its area.
    connection
        .execute(
            "UPDATE player_cards SET region = 'sam' WHERE discord_user_id = '100'",
            [],
        )
        .unwrap();
    // A bridge from before 066, put back, opens a room as it always did, and
    // this bridge's rooms carry their area.
    connection
        .execute_batch(
            "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at,
                 connection_id, creator_region, own_delay)
               VALUES ('older', 'Older bridge', 8, 'build', 'emb_player2', 2000, NULL, 'euc1', 0);
             INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at,
                 connection_id, creator_region, own_delay, creator_area)
               VALUES ('newer', 'This bridge', 8, 'build', 'emb_player3', 3000, NULL, 'use1', 0, 'naw');",
        )
        .unwrap();
    assert_eq!(
        all(
            &connection,
            "SELECT room_id || ':' || COALESCE(creator_area, '-') FROM rooms ORDER BY room_id"
        ),
        ["newer:naw", "older:-", "room:-"]
    );
    drop(connection);

    // Opened again by this bridge, nothing runs twice: the area saved since stays.
    drop(Db::open(&path).unwrap());
    let connection = rusqlite::Connection::open(&path).unwrap();
    let kept: String = connection
        .query_row(
            "SELECT region FROM player_cards WHERE discord_user_id = '100'",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(kept, "sam");
}
