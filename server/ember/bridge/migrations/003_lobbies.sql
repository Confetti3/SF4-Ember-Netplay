-- Lobbies: an opt-in queue that plays one first-to-N set after another on a
-- provider connection. Each set is an ordinary match row.

CREATE TABLE lobbies (
    id TEXT PRIMARY KEY,
    tenant_id TEXT NOT NULL REFERENCES tenants(id),
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    external_lobby_id TEXT NOT NULL,
    create_digest TEXT NOT NULL,
    revision INTEGER NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('open', 'closed')),
    games_to_win INTEGER NOT NULL,
    rotation TEXT NOT NULL CHECK (rotation IN ('winner_stays', 'loser_stays', 'both_rotate')),
    required_build_id TEXT NOT NULL,
    metadata TEXT NOT NULL,
    current_match_id TEXT REFERENCES matches(id),
    sets_started INTEGER NOT NULL,
    sets_completed INTEGER NOT NULL,
    -- Queue order: each join or return to the queue takes the next number.
    next_position INTEGER NOT NULL,
    streak_holder TEXT,
    streak INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE (tenant_id, connection_id, external_lobby_id)
);

-- Everyone who has joined a lobby. `state` is where they are now; a row stays
-- after they leave so the lobby's events remain theirs to read.
CREATE TABLE lobby_entries (
    lobby_id TEXT NOT NULL REFERENCES lobbies(id),
    ember_id TEXT NOT NULL,
    participant_id TEXT NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('queued', 'seated', 'left')),
    position INTEGER,
    slot INTEGER CHECK (slot IN (0, 1)),
    joined_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (lobby_id, ember_id)
);
CREATE INDEX lobby_entries_identity ON lobby_entries (ember_id, state);
CREATE UNIQUE INDEX lobby_entries_slot ON lobby_entries (lobby_id, slot) WHERE state = 'seated';

ALTER TABLE matches ADD COLUMN lobby_id TEXT REFERENCES lobbies(id);
ALTER TABLE events ADD COLUMN lobby_id TEXT;
CREATE INDEX events_lobby ON events (lobby_id, seq);
