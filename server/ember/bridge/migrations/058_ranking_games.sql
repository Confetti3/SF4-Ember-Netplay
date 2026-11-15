-- Ratings, sittings and ranking choices kept apart by game
-- (docs/design/SKILL_BADGE.md, "Games"): a public room's games are rated in
-- its own game's rating (`rooms.game`), so an SFV Ember player's class says
-- nothing of their USF4 and the other way. Each table gains `game` at the
-- head of its key; every row from before is USF4's, and a row written
-- without one is too, so whatever reads or writes these tables as before
-- reads and writes USF4's. `rated_games` is per room already, and a room has
-- one game.
CREATE TABLE skill_ratings_by_game (
    game TEXT NOT NULL DEFAULT 'usf4',
    discord_user_id TEXT NOT NULL,
    rating REAL NOT NULL,
    deviation REAL NOT NULL,
    volatility REAL NOT NULL,
    band INTEGER NOT NULL,
    played_at INTEGER NOT NULL,
    PRIMARY KEY (game, discord_user_id)
) WITHOUT ROWID;
INSERT INTO skill_ratings_by_game
    (game, discord_user_id, rating, deviation, volatility, band, played_at)
    SELECT 'usf4', discord_user_id, rating, deviation, volatility, band, played_at
    FROM skill_ratings;
DROP TABLE skill_ratings;
ALTER TABLE skill_ratings_by_game RENAME TO skill_ratings;

CREATE TABLE rated_pairs_by_game (
    game TEXT NOT NULL DEFAULT 'usf4',
    first TEXT NOT NULL,
    second TEXT NOT NULL,
    games INTEGER NOT NULL,
    played_at INTEGER NOT NULL,
    PRIMARY KEY (game, first, second)
) WITHOUT ROWID;
INSERT INTO rated_pairs_by_game (game, first, second, games, played_at)
    SELECT 'usf4', first, second, games, played_at FROM rated_pairs;
DROP TABLE rated_pairs;
ALTER TABLE rated_pairs_by_game RENAME TO rated_pairs;

-- Taking part, and the board's listing, are each game's: the name and flag
-- a game sends list the player on that game's board, and stopping in one
-- game leaves the other as it was.
CREATE TABLE ranking_choices_by_game (
    game TEXT NOT NULL DEFAULT 'usf4',
    discord_user_id TEXT NOT NULL,
    ranked INTEGER NOT NULL,
    changed_at INTEGER,
    name TEXT,
    country TEXT,
    PRIMARY KEY (game, discord_user_id)
) WITHOUT ROWID;
INSERT INTO ranking_choices_by_game (game, discord_user_id, ranked, changed_at, name, country)
    SELECT 'usf4', discord_user_id, ranked, changed_at, name, country FROM ranking_choices;
DROP TABLE ranking_choices;
ALTER TABLE ranking_choices_by_game RENAME TO ranking_choices;
