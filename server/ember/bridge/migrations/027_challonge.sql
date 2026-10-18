-- Challonge accounts players connected to their Ember IDs through
-- Challonge's own sign-in (routes::sign_in), as discord_accounts. One
-- account per Ember ID and one Ember ID per account; the latest sign-in
-- replaces both. Bracket connectors find players by the username a
-- tournament entry shows, which Challonge compares without case:
-- username_key is its lowercase form, as it was when the account connected.
CREATE TABLE challonge_accounts (
    user_id TEXT PRIMARY KEY,
    ember_id TEXT NOT NULL UNIQUE REFERENCES identities(ember_id) ON DELETE CASCADE,
    username TEXT NOT NULL,
    username_key TEXT NOT NULL,
    connected_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX challonge_accounts_username ON challonge_accounts (username_key);

-- Sign-ins sent to Challonge and not yet answered, as discord_sign_ins: a
-- keyed hash of the state the browser carries (or, once the page asks, of
-- its question's own secret), the Ember ID that asked, when it lapses,
-- whether an answer claimed it, and the account the page asks about with
-- the Ember ID it was connected to then.
CREATE TABLE challonge_sign_ins (
    state_hash BLOB PRIMARY KEY,
    ember_id TEXT NOT NULL,
    expires_at INTEGER NOT NULL,
    claimed INTEGER NOT NULL DEFAULT 0,
    pending_user_id TEXT,
    pending_username TEXT,
    pending_owner TEXT
) WITHOUT ROWID;
CREATE INDEX challonge_sign_ins_expiry ON challonge_sign_ins (expires_at);

-- Connections where the player removed the link their Challonge account's
-- sign-in approved, as discord_withdrawals.
CREATE TABLE challonge_withdrawals (
    user_id TEXT NOT NULL REFERENCES challonge_accounts(user_id) ON DELETE CASCADE,
    connection_id TEXT NOT NULL,
    PRIMARY KEY (user_id, connection_id)
) WITHOUT ROWID;

-- Requests the bridge made to a site with a monthly budget (Challonge's
-- monthly_request_budget), by calendar month in UTC ('YYYY-MM'); warned
-- records that the month's 80% warning was logged.
CREATE TABLE site_requests (
    site TEXT NOT NULL,
    month TEXT NOT NULL,
    spent INTEGER NOT NULL,
    warned INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (site, month)
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
