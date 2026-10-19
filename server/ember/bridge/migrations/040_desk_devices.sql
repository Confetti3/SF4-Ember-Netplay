-- Signing in on another device the other way round (routes::sign_in::desk):
-- that device's browser asks for a short code, kept with a secret only that
-- browser holds (a cookie); the game approves the code with its signed
-- proof, and only that browser can then take the desk session. A code seen
-- on a stream is no use to anyone else. Hashes only, for two minutes. The
-- game no longer shows a code, so a handoff is a link's token alone.
CREATE TABLE desk_devices (
    secret_hash BLOB PRIMARY KEY,
    code_hash BLOB NOT NULL UNIQUE,
    ember_id TEXT REFERENCES identities(ember_id) ON DELETE CASCADE,
    expires_at INTEGER NOT NULL
) WITHOUT ROWID;

DROP TABLE desk_handoffs;
CREATE TABLE desk_handoffs (
    token_hash BLOB PRIMARY KEY,
    ember_id TEXT NOT NULL REFERENCES identities(ember_id) ON DELETE CASCADE,
    expires_at INTEGER NOT NULL
) WITHOUT ROWID;
