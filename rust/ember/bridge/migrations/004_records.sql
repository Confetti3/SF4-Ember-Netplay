-- Records and standings are computed from matches and their accepted games
-- when they are read. The one exception is a lobby player's best streak,
-- which depends on the order sets were played and is kept as it happens; a
-- correction cannot change it because a finished lobby set cannot be reopened
-- and voiding a game never changes who won a finished set.

ALTER TABLE lobby_entries ADD COLUMN best_streak INTEGER NOT NULL DEFAULT 0;
