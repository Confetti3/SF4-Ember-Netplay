-- Fighter levels (docs/design/FIGHTER_LEVELS.md): each fighter's counted
-- games for a Discord account. Points are games + wins; the level is worked
-- out from them when served, so the curve can change without a data change.
-- Kept for good, at most 44 rows an account.
CREATE TABLE fighter_levels (
    discord_user_id TEXT NOT NULL,
    fighter INTEGER NOT NULL CHECK (fighter BETWEEN 0 AND 43),
    games INTEGER NOT NULL,
    wins INTEGER NOT NULL CHECK (wins <= games),
    first_at INTEGER NOT NULL,
    played_at INTEGER NOT NULL,
    PRIMARY KEY (discord_user_id, fighter)
) WITHOUT ROWID;

-- Two Discord accounts' record against each other: the wins of each and when
-- they last played, every counted game from this migration on. Kept for good,
-- one row a pair that ever played. `first` sorts before `second`. The sitting
-- stays in `rated_pairs`, which bridges before this one sweep, so the record
-- has its own table.
CREATE TABLE pair_records (
    first TEXT NOT NULL,
    second TEXT NOT NULL,
    first_wins INTEGER NOT NULL,
    second_wins INTEGER NOT NULL,
    played_at INTEGER NOT NULL,
    PRIMARY KEY (first, second)
) WITHOUT ROWID;

-- A game's place in its battle joins the key: the games of a rematch played
-- in place share their table and generation. Every game rated before is its
-- battle's first. `game` defaults to 0 so a bridge from before this migration
-- still writes the key it always wrote.
CREATE TABLE rated_games_next (
    room_id TEXT NOT NULL REFERENCES rooms(room_id) ON DELETE CASCADE,
    table_id INTEGER NOT NULL,
    generation INTEGER NOT NULL,
    game INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (room_id, table_id, generation, game)
) WITHOUT ROWID;
-- Only the rows whose room still exists: the new table's foreign key is
-- checked as each row lands, and a bridge can hold a rated game whose room was
-- deleted while foreign keys were off. That game has nothing to cascade from,
-- and one such row would fail the copy and stop the bridge from starting.
INSERT INTO rated_games_next (room_id, table_id, generation, game)
    SELECT room_id, table_id, generation, 0 FROM rated_games
    WHERE room_id IN (SELECT room_id FROM rooms);
DROP TABLE rated_games;
ALTER TABLE rated_games_next RENAME TO rated_games;
