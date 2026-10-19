-- The web desk's browser sessions (routes::sign_in::desk): an organizer
-- signs in on the web with a site account their Ember ID already holds, and
-- the browser keeps a cookie stored here only as a keyed hash. A sign-in
-- waiting for the site's answer is kept by the keyed hash of its state.
CREATE TABLE desk_sessions (
    token_hash BLOB PRIMARY KEY,
    ember_id TEXT NOT NULL REFERENCES identities(ember_id) ON DELETE CASCADE,
    expires_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX desk_sessions_expiry ON desk_sessions (expires_at);

CREATE TABLE desk_sign_ins (
    site TEXT NOT NULL,
    state_hash BLOB NOT NULL,
    expires_at INTEGER NOT NULL,
    PRIMARY KEY (site, state_hash)
) WITHOUT ROWID;
