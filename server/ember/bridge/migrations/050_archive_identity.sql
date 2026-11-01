-- The public replay archive (docs/design/REPLAY_ARCHIVE.md), rebuilt.
--
-- A game is known by the SHA-256 of its replay (`replays::Replay`), which
-- two games never share, and kept under it in the bucket. Its CRC is only
-- the native name it travels under. The games 049 kept are not carried
-- over: a stored one no longer holds its replay, so its digest cannot be
-- known, and no bridge has run an archive yet. Tournament games come back
-- through the backfill (`match_replays.archived`) while their sets keep
-- them.
DROP TABLE archive_games;
CREATE TABLE archive_games (
    digest TEXT PRIMARY KEY,
    crc TEXT NOT NULL,
    saved_at INTEGER NOT NULL,
    size INTEGER NOT NULL,
    fighter_a INTEGER NOT NULL,
    fighter_b INTEGER NOT NULL,
    name_a TEXT,
    name_b TEXT,
    country_a TEXT,
    country_b TEXT,
    match_id TEXT,
    event_title TEXT,
    round_label TEXT,
    phase TEXT,
    sent_by TEXT NOT NULL,
    -- 1 for a game its sender's daily allowance paid for: one sent as an
    -- online game, whatever set it joins later.
    charged INTEGER NOT NULL DEFAULT 0,
    priority INTEGER NOT NULL,
    replay BLOB,
    state TEXT NOT NULL DEFAULT 'pending',
    attempts INTEGER NOT NULL DEFAULT 0,
    next_attempt_at INTEGER NOT NULL,
    first_attempt_at INTEGER,
    last_error TEXT,
    received_at INTEGER NOT NULL,
    stored_at INTEGER
) WITHOUT ROWID;
CREATE INDEX archive_games_due ON archive_games (state, priority, next_attempt_at);
-- The listing pages through stored games by (saved_at, digest), newest
-- first, all of them or one event's.
CREATE INDEX archive_games_listed ON archive_games (state, saved_at, digest);
CREATE INDEX archive_games_event ON archive_games (state, event_title, saved_at, digest);
CREATE INDEX archive_games_charged ON archive_games (sent_by, received_at) WHERE charged = 1;

-- Each archived set's pack, `sets/{match_id}.emberpack`. `version` counts
-- the games the set has gained in the archive; the pack queue uploads the
-- set's games as they stand and records the version it read in
-- `uploaded_version`, so the set stays due until the bucket holds its
-- latest games. Its replays stay on the bridge until then. `pruned_at`
-- is when replay retention removed the set's replays: the set is closed
-- then, and takes no more games, so no pack is built from part of it.
CREATE TABLE archive_sets (
    match_id TEXT PRIMARY KEY REFERENCES matches(id),
    version INTEGER NOT NULL,
    uploaded_version INTEGER NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0,
    next_attempt_at INTEGER NOT NULL,
    first_attempt_at INTEGER,
    last_error TEXT,
    pruned_at INTEGER
) WITHOUT ROWID;
CREATE INDEX archive_sets_due ON archive_sets (next_attempt_at) WHERE uploaded_version < version;

-- The listing's filter choices: every event and country of a stored game,
-- added as each game is stored or joins a set.
CREATE TABLE archive_events (
    title TEXT PRIMARY KEY,
    last_saved_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX archive_events_recent ON archive_events (last_saved_at);
CREATE TABLE archive_countries (
    code TEXT PRIMARY KEY
) WITHOUT ROWID;

-- Each set replay's confirmed game, which takes no other replay (none for
-- one kept before games were told apart), and whether the archive has
-- taken it in: the backfill takes the rest.
ALTER TABLE match_replays ADD COLUMN attempt_id TEXT REFERENCES attempts(id);
ALTER TABLE match_replays ADD COLUMN archived INTEGER NOT NULL DEFAULT 0;
CREATE UNIQUE INDEX match_replays_attempt ON match_replays (attempt_id) WHERE attempt_id IS NOT NULL;
CREATE INDEX match_replays_unarchived ON match_replays (received_at) WHERE archived = 0;
