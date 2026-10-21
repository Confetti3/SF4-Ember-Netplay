-- Tournament Mode by invitation (routes::organizer::invites): the codes the
-- operator gave out, each good once, and the Ember IDs that may organize.
CREATE TABLE organizer_invites (
    code_hash BLOB PRIMARY KEY,
    created_at INTEGER NOT NULL,
    note TEXT,
    redeemed_by TEXT,
    redeemed_at INTEGER
) WITHOUT ROWID;
CREATE TABLE organizers (
    ember_id TEXT PRIMARY KEY,
    since INTEGER NOT NULL,
    code_hash BLOB
) WITHOUT ROWID;
