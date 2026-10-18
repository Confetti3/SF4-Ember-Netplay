-- Accounts players connected to their Ember IDs on sign-in sites
-- (routes::sign_in, the sign-in sites design GENERIC_SITES.md), one set of
-- tables for every configured site, so a new site needs no migration.
-- Discord keeps its own tables. On each site one account per Ember ID and
-- one Ember ID per account; the latest sign-in replaces both. username_key
-- is the username as the site's username_rule compares it (lowercase when
-- it ignores case), kept for a site whose lookups match usernames (NULL
-- otherwise), and NULL too once another account of that site connected
-- with that name, so at most one account answers for a name.
CREATE TABLE site_accounts (
    site TEXT NOT NULL,
    user_id TEXT NOT NULL,
    ember_id TEXT NOT NULL REFERENCES identities(ember_id) ON DELETE CASCADE,
    username TEXT NOT NULL,
    username_key TEXT,
    connected_at INTEGER NOT NULL,
    PRIMARY KEY (site, user_id),
    UNIQUE (site, ember_id)
) WITHOUT ROWID;
CREATE UNIQUE INDEX site_accounts_username ON site_accounts (site, username_key)
    WHERE username_key IS NOT NULL;

-- Sign-ins sent to a site and not yet answered, as discord_sign_ins: a keyed
-- hash of the state the browser carries (or, once the page asks, of its
-- question's own secret), the Ember ID that asked, when it lapses, whether
-- an answer claimed it, and the account the page asks about with the Ember
-- ID it was connected to then.
CREATE TABLE site_sign_ins (
    site TEXT NOT NULL,
    state_hash BLOB NOT NULL,
    ember_id TEXT NOT NULL,
    expires_at INTEGER NOT NULL,
    claimed INTEGER NOT NULL DEFAULT 0,
    pending_user_id TEXT,
    pending_username TEXT,
    pending_owner TEXT,
    PRIMARY KEY (site, state_hash)
) WITHOUT ROWID;
CREATE INDEX site_sign_ins_ember ON site_sign_ins (site, ember_id);
CREATE INDEX site_sign_ins_expiry ON site_sign_ins (expires_at);

-- Connections where the player removed the link their site account's
-- sign-in approved, as discord_withdrawals.
CREATE TABLE site_withdrawals (
    site TEXT NOT NULL,
    user_id TEXT NOT NULL,
    connection_id TEXT NOT NULL,
    PRIMARY KEY (site, user_id, connection_id),
    FOREIGN KEY (site, user_id) REFERENCES site_accounts (site, user_id) ON DELETE CASCADE
) WITHOUT ROWID;

-- Requests the bridge made to a site with a request budget, by day in UTC
-- ('YYYY-MM-DD'); a month's spending is the sum of its days.
CREATE TABLE site_requests (
    site TEXT NOT NULL,
    day TEXT NOT NULL,
    spent INTEGER NOT NULL,
    PRIMARY KEY (site, day)
) WITHOUT ROWID;

-- A link a site's sign-in approved: approved_via 'site', with
-- '<site>:<user ID>' as the evidence in claim_id. Rebuilt as in version 9,
-- since SQLite cannot change a CHECK; nothing references the table.
CREATE TABLE links_v27 (
    id TEXT PRIMARY KEY,
    account_id TEXT NOT NULL REFERENCES external_accounts(id),
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    ember_id TEXT NOT NULL REFERENCES identities(ember_id),
    approved_via TEXT NOT NULL CHECK (approved_via IN ('browser', 'provider_proxy', 'discord', 'site')),
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
