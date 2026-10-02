-- Discord accounts players connected to their Ember IDs through Discord's own
-- sign-in. One account per Ember ID and one Ember ID per account; the latest
-- sign-in replaces both. Platforms that find players by Discord account read
-- it, so a player's Ember ID is found without a code.
CREATE TABLE discord_accounts (
    user_id TEXT PRIMARY KEY,
    ember_id TEXT NOT NULL UNIQUE REFERENCES identities(ember_id),
    username TEXT NOT NULL,
    connected_at INTEGER NOT NULL
) WITHOUT ROWID;

-- Sign-ins sent to Discord and not yet answered: a keyed hash of the state
-- the browser carries, the Ember ID that asked, and when it lapses.
CREATE TABLE discord_sign_ins (
    state_hash BLOB PRIMARY KEY,
    ember_id TEXT NOT NULL,
    expires_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX discord_sign_ins_expiry ON discord_sign_ins(expires_at);
