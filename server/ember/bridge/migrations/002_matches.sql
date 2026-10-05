-- Logical matches, their frozen rosters, attempts and adjudications.

CREATE TABLE matches (
    id TEXT PRIMARY KEY,
    tenant_id TEXT NOT NULL REFERENCES tenants(id),
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    external_match_id TEXT NOT NULL,
    create_digest TEXT NOT NULL,
    revision INTEGER NOT NULL,
    assignment_generation INTEGER NOT NULL,
    state TEXT NOT NULL,
    games_to_win INTEGER NOT NULL,
    rules TEXT NOT NULL,
    required_build_id TEXT NOT NULL,
    metadata TEXT NOT NULL,
    delivery_state TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE (tenant_id, connection_id, external_match_id)
);

CREATE TABLE match_participants (
    match_id TEXT NOT NULL REFERENCES matches(id),
    assignment_generation INTEGER NOT NULL,
    slot INTEGER NOT NULL CHECK (slot IN (0, 1)),
    participant_id TEXT NOT NULL,
    ember_id TEXT NOT NULL,
    PRIMARY KEY (match_id, assignment_generation, slot),
    UNIQUE (match_id, assignment_generation, ember_id)
);
CREATE INDEX match_participants_identity ON match_participants (ember_id);

CREATE TABLE adjudications (
    id TEXT PRIMARY KEY,
    match_id TEXT NOT NULL REFERENCES matches(id),
    actor TEXT NOT NULL,
    kind TEXT NOT NULL,
    reason TEXT NOT NULL,
    evidence TEXT NOT NULL,
    expected_revision INTEGER NOT NULL,
    result TEXT NOT NULL,
    created_at INTEGER NOT NULL
);

-- One row per game attempt. `seq` makes a second scoring of the same game
-- impossible: each attempt has exactly one score effect.
CREATE TABLE attempts (
    id TEXT PRIMARY KEY,
    match_id TEXT NOT NULL REFERENCES matches(id),
    assignment_generation INTEGER NOT NULL,
    seq INTEGER NOT NULL,
    outcome TEXT NOT NULL CHECK (outcome IN ('p1_win', 'p2_win', 'draw')),
    source TEXT NOT NULL CHECK (source IN ('organizer_adjudication')),
    state TEXT NOT NULL CHECK (state IN ('accepted', 'voided')),
    adjudication_id TEXT NOT NULL REFERENCES adjudications(id),
    voided_by TEXT,
    created_at INTEGER NOT NULL,
    UNIQUE (match_id, seq)
);
