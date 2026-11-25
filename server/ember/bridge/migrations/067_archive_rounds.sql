-- Each archived game's final round score (docs/design/REPLAY_ARCHIVE.md,
-- "Rounds"), which the listing shows so the page can say who won.
--
-- * `rounds_a` / `rounds_b`: the rounds each side won
--   (`replays::Replay::rounds`), both or neither; NULL when the replay does
--   not say who won.
-- * `rounds_read_at`: when the bridge read them from the game's replay; NULL
--   while it has not. `archive::admit` reads a new game's at once. The games
--   kept before this migration have none, and `delivery::rounds` reads them
--   back from the bucket, a few at a time, newest first; the index holds the
--   stored games still to read, so it empties as the backfill finishes.
--
-- Nullable columns with no default only change the table's definition, so
-- adding them is cheap on the live table. No trigger or view reads them, so
-- the listing's index (migration 055) is untouched. A bridge from before this
-- one, put back, names its columns and leaves all three NULL: its new games
-- wait for the backfill once this one runs again.
ALTER TABLE archive_games ADD COLUMN rounds_a INTEGER;
ALTER TABLE archive_games ADD COLUMN rounds_b INTEGER;
ALTER TABLE archive_games ADD COLUMN rounds_read_at INTEGER;
CREATE INDEX archive_games_rounds_unread ON archive_games (saved_at)
    WHERE rounds_read_at IS NULL AND state = 'stored';
