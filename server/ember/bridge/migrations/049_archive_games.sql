-- The public replay archive (docs/design/REPLAY_ARCHIVE.md): every game a
-- player sent, kept once by its CRC, with what the archive page filters on:
-- the two sides' names, countries and fighters (native IDs), and for a
-- tournament set its event and round. The replay is held here until the
-- archive's bucket stores it, tournament games first (priority 0), then
-- dropped. `state` is pending until stored.
CREATE TABLE archive_games (
    crc TEXT PRIMARY KEY,
    saved_at INTEGER NOT NULL,
    size INTEGER NOT NULL,
    fighter_a INTEGER,
    fighter_b INTEGER,
    name_a TEXT,
    name_b TEXT,
    country_a TEXT,
    country_b TEXT,
    match_id TEXT,
    event_title TEXT,
    round_label TEXT,
    phase TEXT,
    sent_by TEXT NOT NULL,
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
CREATE INDEX archive_games_listed ON archive_games (state, saved_at);
CREATE INDEX archive_games_sender ON archive_games (sent_by, received_at);
