-- Challonge accounts players connected to their Ember IDs through
-- Challonge's device sign-in (routes::challonge). One account per Ember ID
-- and one Ember ID per account; the latest sign-in replaces both. Bracket
-- connectors find players by the username a tournament entry shows, which
-- Challonge compares without case: username_key is its lowercase form.
CREATE TABLE challonge_accounts (
    user_id TEXT PRIMARY KEY,
    ember_id TEXT NOT NULL UNIQUE REFERENCES identities(ember_id) ON DELETE CASCADE,
    username TEXT NOT NULL,
    username_key TEXT NOT NULL,
    connected_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX challonge_accounts_username ON challonge_accounts (username_key);

-- The one sign-in each Ember ID has in progress or just ended. While it is
-- waiting, device_code is what the bridge polls Challonge's token endpoint
-- with, no sooner than next_poll_at; it is cleared once the sign-in leaves
-- waiting. In moving, the account that signed in is held with the Ember ID
-- it was connected to then, until the player moves it or keeps it there; in
-- held, only its username is kept, to show.
CREATE TABLE challonge_sign_ins (
    ember_id TEXT PRIMARY KEY,
    state TEXT NOT NULL CHECK (state IN ('waiting', 'moving', 'held', 'denied', 'expired', 'failed')),
    device_code TEXT,
    user_code TEXT NOT NULL,
    verification_uri TEXT NOT NULL,
    verification_uri_complete TEXT,
    expires_at INTEGER NOT NULL,
    interval_secs INTEGER NOT NULL,
    next_poll_at INTEGER NOT NULL,
    pending_user_id TEXT,
    pending_username TEXT,
    pending_owner TEXT
) WITHOUT ROWID;
CREATE INDEX challonge_sign_ins_due ON challonge_sign_ins (state, next_poll_at);
CREATE INDEX challonge_sign_ins_expiry ON challonge_sign_ins (expires_at);

-- Requests the bridge made to Challonge, by calendar month (UTC,
-- 'YYYY-MM'), against the configured monthly budget; warned records that the
-- month's 80% warning was logged.
CREATE TABLE challonge_requests (
    month TEXT PRIMARY KEY,
    spent INTEGER NOT NULL,
    warned INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;

-- When each Ember ID last opened a player session. Session rows go a day
-- after they lapse, so this keeps the one fact a Challonge move needs:
-- whether the Ember ID holding an account is still in use. Before this
-- version, the newest session still stored, or the Ember ID's creation.
ALTER TABLE identities ADD COLUMN last_session_at INTEGER NOT NULL DEFAULT 0;
UPDATE identities SET last_session_at = MAX(created_at,
    COALESCE((SELECT MAX(s.created_at) FROM sessions s WHERE s.ember_id = identities.ember_id), 0));

-- Connections where the player removed the link their Challonge account's
-- sign-in approved, as discord_withdrawals.
CREATE TABLE challonge_withdrawals (
    user_id TEXT NOT NULL REFERENCES challonge_accounts(user_id) ON DELETE CASCADE,
    connection_id TEXT NOT NULL,
    PRIMARY KEY (user_id, connection_id)
) WITHOUT ROWID;

-- A link a Challonge sign-in approved: approved_via 'challonge', with
-- 'challonge:<user ID>' as the evidence in claim_id. Rebuilt as in version 9,
-- since SQLite cannot change a CHECK; nothing references the table.
CREATE TABLE links_v27 (
    id TEXT PRIMARY KEY,
    account_id TEXT NOT NULL REFERENCES external_accounts(id),
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    ember_id TEXT NOT NULL REFERENCES identities(ember_id),
    approved_via TEXT NOT NULL CHECK (approved_via IN ('browser', 'provider_proxy', 'discord', 'challonge')),
    claim_id TEXT NOT NULL,
    consented_at INTEGER NOT NULL,
    approved_at INTEGER NOT NULL,
    revoked_at INTEGER,
    revoked_by TEXT
);
INSERT INTO links_v27 SELECT id, account_id, connection_id, ember_id, approved_via, claim_id,
    consented_at, approved_at, revoked_at, revoked_by FROM links;
DROP TABLE links;
ALTER TABLE links_v27 RENAME TO links;
CREATE UNIQUE INDEX links_active_account ON links (account_id) WHERE revoked_at IS NULL;
CREATE UNIQUE INDEX links_active_identity ON links (ember_id, connection_id) WHERE revoked_at IS NULL;
