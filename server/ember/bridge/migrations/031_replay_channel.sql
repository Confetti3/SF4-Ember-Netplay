-- The replay channel (docs/design/REPLAY_CHANNEL.md): room games shared with
-- everyone, and each Discord account's choice to share them.

-- Whether a Discord account shares its games. Without a row it does.
CREATE TABLE replay_sharing (
    discord_user_id TEXT PRIMARY KEY,
    sharing INTEGER NOT NULL,
    changed_at INTEGER NOT NULL
) WITHOUT ROWID;

-- Every public room game a room host reported, with both players' Ember IDs,
-- kept 14 days. Not tied to the room's row: `rated_games` goes with the room,
-- and a replay can be uploaded after the room has closed. Their Discord
-- accounts are looked up when an upload comes. The two players' classes (as
-- JSON, none for a player with no class to show or no account) are the ones
-- they held when the host reported the game, so an upload long after it still
-- lists the class at the game.
CREATE TABLE channel_games (
    room_id TEXT NOT NULL,
    table_id INTEGER NOT NULL,
    generation INTEGER NOT NULL,
    winner_id TEXT NOT NULL,
    loser_id TEXT NOT NULL,
    winner_badge TEXT,
    loser_badge TEXT,
    played_at INTEGER NOT NULL,
    PRIMARY KEY (room_id, table_id, generation)
) WITHOUT ROWID;

CREATE INDEX channel_games_played ON channel_games (played_at);

-- One row per game with an uploaded replay. Side 0 and 1 are the game's
-- player slots. A row is held (`published` 0) until both sides' accounts are
-- known: a public room's host report and both players' accounts when it was
-- uploaded, or the other player's upload of the same private room game. A
-- side's name and country come from that side's own upload; `uploaded` says
-- it has made one. `badge` is the side's class as JSON at the game (a
-- public room's: when the host reported it; a private room's: when that side
-- uploaded), and `band` its class number for filtering (none for the developer
-- mark). `seq` orders entries stably for paging; `id` is what is shown.
CREATE TABLE channel_replays (
    seq INTEGER PRIMARY KEY,
    id TEXT NOT NULL UNIQUE,
    room_id TEXT NOT NULL,
    table_id INTEGER NOT NULL,
    generation INTEGER NOT NULL,
    room_kind TEXT NOT NULL,
    account0 TEXT,
    account1 TEXT,
    name0 TEXT,
    name1 TEXT,
    country0 TEXT,
    country1 TEXT,
    uploaded0 INTEGER NOT NULL,
    uploaded1 INTEGER NOT NULL,
    badge0 TEXT,
    badge1 TEXT,
    band0 INTEGER,
    band1 INTEGER,
    fighter0 INTEGER NOT NULL,
    fighter1 INTEGER NOT NULL,
    winner INTEGER,
    played_at INTEGER NOT NULL,
    sha256 TEXT NOT NULL,
    size INTEGER NOT NULL,
    published INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE (room_id, table_id, generation)
);

CREATE INDEX channel_replays_listed ON channel_replays (played_at) WHERE published = 1;
CREATE INDEX channel_replays_fighter0 ON channel_replays (fighter0, played_at) WHERE published = 1;
CREATE INDEX channel_replays_fighter1 ON channel_replays (fighter1, played_at) WHERE published = 1;
CREATE INDEX channel_replays_held ON channel_replays (created_at) WHERE published = 0;
CREATE INDEX channel_replays_sha ON channel_replays (sha256);
