-- Browser handoffs (spec 12.1): one-use, 60-second codes a provider or
-- organizer mints for one assigned player. Only a keyed hash of the code is
-- kept; the expected player's identity redeems it once.
CREATE TABLE handoffs (
    code_hash BLOB PRIMARY KEY,
    match_id TEXT NOT NULL REFERENCES matches(id),
    ember_id TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    redeemed_at INTEGER
) WITHOUT ROWID;
CREATE INDEX handoffs_player ON handoffs(match_id, ember_id);
CREATE INDEX handoffs_expiry ON handoffs(expires_at);
