-- Account points (docs/design/FIGHTER_LEVELS.md): each Discord account's own
-- counted games, every counted game whether or not it named fighters. Points
-- are games + wins, worked out when served, as for `fighter_levels`. Kept for
-- good, one row an account that ever played.
CREATE TABLE account_points (
    discord_user_id TEXT PRIMARY KEY,
    games INTEGER NOT NULL,
    wins INTEGER NOT NULL CHECK (wins <= games),
    first_at INTEGER NOT NULL,
    played_at INTEGER NOT NULL
) WITHOUT ROWID;
