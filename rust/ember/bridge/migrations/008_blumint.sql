-- Platforms that find players by Discord account (BluMint) get a link the
-- player's Discord sign-in approved: approved_via 'discord', with the Discord
-- user ID as the evidence in claim_id. SQLite cannot change a CHECK, so the
-- table is rebuilt; nothing references it.
CREATE TABLE links_v8 (
    id TEXT PRIMARY KEY,
    account_id TEXT NOT NULL REFERENCES external_accounts(id),
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    ember_id TEXT NOT NULL REFERENCES identities(ember_id),
    approved_via TEXT NOT NULL CHECK (approved_via IN ('browser', 'provider_proxy', 'discord')),
    claim_id TEXT NOT NULL,
    consented_at INTEGER NOT NULL,
    approved_at INTEGER NOT NULL,
    revoked_at INTEGER,
    revoked_by TEXT
);
INSERT INTO links_v8 SELECT id, account_id, connection_id, ember_id, approved_via, claim_id,
    consented_at, approved_at, revoked_at, revoked_by FROM links;
DROP TABLE links;
ALTER TABLE links_v8 RENAME TO links;
CREATE UNIQUE INDEX links_active_account ON links (account_id) WHERE revoked_at IS NULL;
CREATE UNIQUE INDEX links_active_identity ON links (ember_id, connection_id) WHERE revoked_at IS NULL;

-- Results the bridge sends to the platform that created the match
-- (matches.delivery_state, spec 18): how often it tried and when it tries next.
ALTER TABLE matches ADD COLUMN delivery_attempts INTEGER NOT NULL DEFAULT 0;
ALTER TABLE matches ADD COLUMN delivery_next_at INTEGER NOT NULL DEFAULT 0;
CREATE INDEX matches_delivery ON matches (delivery_state, delivery_next_at);
