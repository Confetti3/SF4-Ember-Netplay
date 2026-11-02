-- One row per game, however many players send it (docs/design/REPLAY_ARCHIVE.md,
-- "Which game it is"). `game` is `replays::Replay::game`, the same for both
-- players' copies, whose digests differ: each PC writes its own values into
-- the replay's header. A new game is taken with it; a game kept before has
-- none until `delivery::duplicates` reads its copy from the bucket. Once no
-- copy is left unknown, maintenance folds the copies of each game kept twice
-- into one (`archive::duplicates`), once: `archive_fold_owed` holds its one
-- row until then. A copy folded away is a `duplicate` until the bucket has
-- deleted its file.
ALTER TABLE archive_games ADD COLUMN game TEXT;
CREATE INDEX archive_games_game ON archive_games (game) WHERE game IS NOT NULL;
CREATE INDEX archive_games_unknown ON archive_games (digest) WHERE game IS NULL;
CREATE INDEX archive_games_unsettled ON archive_games (next_attempt_at)
    WHERE (game IS NULL AND state = 'stored') OR state = 'duplicate';
CREATE TABLE archive_fold_owed (owed INTEGER PRIMARY KEY CHECK (owed = 1));
INSERT INTO archive_fold_owed VALUES (1);
