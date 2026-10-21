-- The hidden ratings behind public room classes (docs/design/SKILL_BADGE.md),
-- one per Discord account: Glicko-2 on the Glicko scale, the class a room
-- ticket last showed (0 before the first; a class moves from it only past a
-- margin) and when the account last played a rated game. Nothing but the
-- class is ever served.
CREATE TABLE skill_ratings (
    discord_user_id TEXT PRIMARY KEY,
    rating REAL NOT NULL,
    deviation REAL NOT NULL,
    volatility REAL NOT NULL,
    band INTEGER NOT NULL,
    played_at INTEGER NOT NULL
) WITHOUT ROWID;

-- Public room games already rated. A room host reports its recent games on
-- every poll; a game counts the first time. Gone with the room's row.
CREATE TABLE rated_games (
    room_id TEXT NOT NULL REFERENCES rooms(room_id) ON DELETE CASCADE,
    table_id INTEGER NOT NULL,
    generation INTEGER NOT NULL,
    PRIMARY KEY (room_id, table_id, generation)
) WITHOUT ROWID;

-- Two accounts' current sitting: how many rated games they have played each
-- other since a gap of three hours, the last at `played_at`. Past eight, a
-- game between them counts at a quarter. `first` sorts before `second`.
CREATE TABLE rated_pairs (
    first TEXT NOT NULL,
    second TEXT NOT NULL,
    games INTEGER NOT NULL,
    played_at INTEGER NOT NULL,
    PRIMARY KEY (first, second)
) WITHOUT ROWID;
