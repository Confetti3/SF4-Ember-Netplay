-- One row per game, however many players send it (docs/design/REPLAY_ARCHIVE.md,
-- "Which game it is"). `game` is `replays::Replay::game`, the same for both
-- players' copies, whose digests differ: each PC writes its own values into
-- the replay's header. A new game is taken with it; a game kept before has
-- none until `archive::duplicates` reads its copy from the bucket, folds it
-- into the copy of the same game that was kept first, and removes the other
-- (state 'duplicate' until the bucket has deleted its file).
ALTER TABLE archive_games ADD COLUMN game TEXT;
CREATE INDEX archive_games_game ON archive_games (game) WHERE game IS NOT NULL;
CREATE INDEX archive_games_unread ON archive_games (state) WHERE game IS NULL OR state = 'duplicate';
