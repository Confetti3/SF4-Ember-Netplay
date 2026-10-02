-- Bridge-run play (spec 14 to 16): the fighters' claims, the match room, the
-- next game each fighter is preparing, and the reports fighters sign.

-- One claim per fighter: the endpoint and build of its current helper run.
CREATE TABLE match_claims (
    match_id TEXT NOT NULL REFERENCES matches(id),
    ember_id TEXT NOT NULL,
    endpoint_id TEXT NOT NULL,
    helper_instance_id TEXT NOT NULL,
    build_id TEXT NOT NULL,
    claimed_at INTEGER NOT NULL,
    PRIMARY KEY (match_id, ember_id)
);

-- The match's provisioning lease and room. `binding_revision` moves whenever
-- the endpoints the binding names change; a replaced room also moves the
-- match's assignment generation.
CREATE TABLE match_rooms (
    match_id TEXT PRIMARY KEY REFERENCES matches(id),
    lease_id TEXT,
    lease_holder TEXT,
    lease_fence INTEGER NOT NULL DEFAULT 0,
    lease_expires_at INTEGER,
    room_id TEXT,
    invitation_sealed BLOB,
    published_by TEXT,
    published_at INTEGER,
    binding_revision INTEGER NOT NULL DEFAULT 0,
    bound_endpoints TEXT
);

-- Each fighter's description of the next game, until both agree.
CREATE TABLE attempt_preparations (
    match_id TEXT NOT NULL REFERENCES matches(id),
    ember_id TEXT NOT NULL,
    descriptor TEXT NOT NULL,
    prepared_at INTEGER NOT NULL,
    PRIMARY KEY (match_id, ember_id)
);

-- Attempts gain the fighters' kind: permitted before any result, decided by
-- two agreeing reports, or held for review. Organizer attempts are unchanged.
CREATE TABLE attempts_v6 (
    id TEXT PRIMARY KEY,
    match_id TEXT NOT NULL REFERENCES matches(id),
    assignment_generation INTEGER NOT NULL,
    seq INTEGER NOT NULL,
    outcome TEXT CHECK (outcome IS NULL OR outcome IN ('p1_win', 'p2_win', 'draw')),
    source TEXT NOT NULL CHECK (source IN ('organizer_adjudication', 'player_agreement')),
    state TEXT NOT NULL CHECK (state IN ('permitted', 'accepted', 'voided', 'aborted', 'review')),
    adjudication_id TEXT REFERENCES adjudications(id),
    voided_by TEXT,
    created_at INTEGER NOT NULL,
    permit_id TEXT UNIQUE,
    permit TEXT,
    match_generation INTEGER,
    start_by INTEGER,
    first_report_at INTEGER,
    UNIQUE (match_id, seq),
    CHECK ((source = 'organizer_adjudication') = (permit_id IS NULL)),
    CHECK (state != 'accepted' OR outcome IS NOT NULL)
);
INSERT INTO attempts_v6 (id, match_id, assignment_generation, seq, outcome, source, state, adjudication_id, voided_by, created_at)
    SELECT id, match_id, assignment_generation, seq, outcome, source, state, adjudication_id, voided_by, created_at FROM attempts;
DROP TABLE attempts;
ALTER TABLE attempts_v6 RENAME TO attempts;
-- At most one open player attempt per match.
CREATE UNIQUE INDEX attempts_open ON attempts (match_id) WHERE state IN ('permitted', 'review');

-- Every valid signed report, kept as evidence. The first report of each
-- fighter for an attempt is the one that counts (spec 16.4).
CREATE TABLE game_reports (
    id TEXT PRIMARY KEY,
    attempt_id TEXT NOT NULL REFERENCES attempts(id),
    match_id TEXT NOT NULL REFERENCES matches(id),
    reporter_id TEXT NOT NULL,
    observation_id TEXT NOT NULL,
    digest TEXT NOT NULL,
    signed TEXT NOT NULL,
    result TEXT NOT NULL,
    received_at INTEGER NOT NULL,
    UNIQUE (attempt_id, reporter_id, observation_id)
);
CREATE INDEX game_reports_attempt ON game_reports (attempt_id, reporter_id, received_at);
