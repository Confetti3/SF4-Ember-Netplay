-- Opening the web desk from the game (routes::sign_in::desk): the game asks
-- for a one-time handoff with its signed proof, and the browser trades it
-- for a desk session marked `from_game`, by the link's token or by the
-- short code the player types. Both are kept only as keyed hashes, for two
-- minutes, and either uses the handoff up.
CREATE TABLE desk_handoffs (
    token_hash BLOB PRIMARY KEY,
    code_hash BLOB NOT NULL UNIQUE,
    ember_id TEXT NOT NULL REFERENCES identities(ember_id) ON DELETE CASCADE,
    expires_at INTEGER NOT NULL
) WITHOUT ROWID;

ALTER TABLE desk_sessions ADD COLUMN from_game INTEGER NOT NULL DEFAULT 0;
