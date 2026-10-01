-- Tournaments: single elimination, double elimination or round robin on a
-- provider connection. The bracket's layout is worked out again from the
-- format and the entrant count whenever it is needed, so only each node's
-- state is stored. Each set is an ordinary match row.

CREATE TABLE tournaments (
    id TEXT PRIMARY KEY,
    tenant_id TEXT NOT NULL REFERENCES tenants(id),
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    external_tournament_id TEXT NOT NULL,
    create_digest TEXT NOT NULL,
    revision INTEGER NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('registration', 'running', 'completed', 'cancelled')),
    format TEXT NOT NULL CHECK (format IN ('single_elimination', 'double_elimination', 'round_robin')),
    games_to_win INTEGER NOT NULL,
    finals_games_to_win INTEGER NOT NULL,
    grand_final_reset INTEGER NOT NULL,
    required_build_id TEXT NOT NULL,
    metadata TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE (tenant_id, connection_id, external_tournament_id)
);

-- `seed` is 0 for the top seed and is set when the tournament starts.
-- `placement` is set when it completes.
CREATE TABLE tournament_entrants (
    tournament_id TEXT NOT NULL REFERENCES tournaments(id),
    ember_id TEXT NOT NULL,
    participant_id TEXT NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('registered', 'withdrawn')),
    seed INTEGER,
    placement INTEGER,
    registered_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (tournament_id, ember_id)
);
CREATE INDEX tournament_entrants_identity ON tournament_entrants (ember_id, state);
CREATE UNIQUE INDEX tournament_entrants_seed ON tournament_entrants (tournament_id, seed) WHERE seed IS NOT NULL;

-- One row per bracket node. A slot is NULL while open, -1 when empty, or the
-- seed of its player. `match_count` numbers the node's matches, because a
-- correction can cancel one and a new match is made later.
CREATE TABLE tournament_nodes (
    tournament_id TEXT NOT NULL REFERENCES tournaments(id),
    node INTEGER NOT NULL,
    slot_a INTEGER,
    slot_b INTEGER,
    status TEXT NOT NULL CHECK (status IN ('pending', 'ready', 'playing', 'played', 'bye', 'walkover', 'skipped')),
    winner_slot INTEGER CHECK (winner_slot IN (0, 1)),
    match_id TEXT REFERENCES matches(id),
    match_count INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (tournament_id, node)
);
CREATE INDEX tournament_nodes_match ON tournament_nodes (match_id);

ALTER TABLE matches ADD COLUMN tournament_id TEXT REFERENCES tournaments(id);
ALTER TABLE events ADD COLUMN tournament_id TEXT;
CREATE INDEX events_tournament ON events (tournament_id, seq);
