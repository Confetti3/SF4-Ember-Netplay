-- Identity, linking, events and delivery (spec 23.1). Plain SQL with partial
-- unique indexes, which SQLite and PostgreSQL both support.

CREATE TABLE tenants (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL
);

CREATE TABLE provider_connections (
    id TEXT PRIMARY KEY,
    tenant_id TEXT NOT NULL REFERENCES tenants(id),
    kind TEXT NOT NULL,
    environment TEXT NOT NULL,
    display_name TEXT NOT NULL,
    enabled INTEGER NOT NULL
);

-- Provider and organizer API credentials, stored only as keyed hashes.
CREATE TABLE service_credentials (
    token_hash BLOB PRIMARY KEY,
    id TEXT NOT NULL UNIQUE,
    tenant_id TEXT NOT NULL REFERENCES tenants(id),
    connection_id TEXT REFERENCES provider_connections(id),
    role TEXT NOT NULL CHECK (role IN ('provider', 'organizer')),
    label TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    revoked_at INTEGER
);

CREATE TABLE identities (
    ember_id TEXT PRIMARY KEY,
    public_key TEXT NOT NULL UNIQUE,
    created_at INTEGER NOT NULL
);

CREATE TABLE external_accounts (
    id TEXT PRIMARY KEY,
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    subject TEXT NOT NULL,
    participant_id TEXT NOT NULL UNIQUE,
    display_label TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE (connection_id, subject)
);

CREATE TABLE links (
    id TEXT PRIMARY KEY,
    account_id TEXT NOT NULL REFERENCES external_accounts(id),
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    ember_id TEXT NOT NULL REFERENCES identities(ember_id),
    approved_via TEXT NOT NULL CHECK (approved_via IN ('browser', 'provider_proxy')),
    claim_id TEXT NOT NULL,
    consented_at INTEGER NOT NULL,
    approved_at INTEGER NOT NULL,
    revoked_at INTEGER,
    revoked_by TEXT
);
CREATE UNIQUE INDEX links_active_account ON links (account_id) WHERE revoked_at IS NULL;
CREATE UNIQUE INDEX links_active_identity ON links (ember_id, connection_id) WHERE revoked_at IS NULL;

CREATE TABLE browser_sessions (
    token_hash BLOB PRIMARY KEY,
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    account_id TEXT NOT NULL REFERENCES external_accounts(id),
    authenticated_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL
);

CREATE TABLE link_intents (
    id TEXT PRIMARY KEY,
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    account_id TEXT NOT NULL REFERENCES external_accounts(id),
    -- HMAC of the normalized code; the code itself is never stored.
    code_hmac BLOB NOT NULL UNIQUE,
    created_via TEXT NOT NULL CHECK (created_via IN ('browser', 'provider')),
    browser_session BLOB,
    state TEXT NOT NULL CHECK (state IN ('created', 'claim_pending', 'approved', 'expired', 'cancelled')),
    failed_attempts INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL
);

CREATE TABLE link_claims (
    id TEXT PRIMARY KEY,
    intent_id TEXT NOT NULL REFERENCES link_intents(id),
    ember_id TEXT NOT NULL REFERENCES identities(ember_id),
    challenge_id TEXT NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('pending', 'approved', 'denied', 'cancelled', 'expired')),
    consented_at INTEGER NOT NULL,
    decided_at INTEGER
);
CREATE UNIQUE INDEX link_claims_live ON link_claims (intent_id) WHERE state = 'pending';

CREATE TABLE auth_challenges (
    id TEXT PRIMARY KEY,
    ember_id TEXT NOT NULL,
    public_key TEXT NOT NULL,
    canonical BLOB NOT NULL,
    expires_at INTEGER NOT NULL,
    consumed_at INTEGER
);

CREATE TABLE sessions (
    token_hash BLOB PRIMARY KEY,
    ember_id TEXT NOT NULL REFERENCES identities(ember_id),
    scopes TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    revoked_at INTEGER
);

CREATE TABLE idempotency_records (
    actor TEXT NOT NULL,
    method TEXT NOT NULL,
    path TEXT NOT NULL,
    idempotency_key TEXT NOT NULL,
    command_digest TEXT NOT NULL,
    status INTEGER NOT NULL,
    response BLOB NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (actor, method, path, idempotency_key)
);

CREATE TABLE audit_entries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    at INTEGER NOT NULL,
    actor_class TEXT NOT NULL,
    actor_id TEXT NOT NULL,
    action TEXT NOT NULL,
    target TEXT NOT NULL,
    result TEXT NOT NULL,
    reason TEXT
);

-- Events are stored as the exact bytes every delivery sends.
CREATE TABLE events (
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    id TEXT NOT NULL UNIQUE,
    tenant_id TEXT NOT NULL REFERENCES tenants(id),
    connection_id TEXT,
    type TEXT NOT NULL,
    subject TEXT NOT NULL,
    match_id TEXT,
    ember_id TEXT,
    body BLOB NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE INDEX events_tenant ON events (tenant_id, seq);
CREATE INDEX events_match ON events (match_id, seq);
CREATE INDEX events_identity ON events (ember_id, seq);

CREATE TABLE webhook_subscriptions (
    id TEXT PRIMARY KEY,
    tenant_id TEXT NOT NULL REFERENCES tenants(id),
    owner_credential TEXT NOT NULL,
    connection_id TEXT,
    url TEXT NOT NULL,
    event_types TEXT NOT NULL,
    -- Secrets are sealed with the bridge's encryption key.
    secret_sealed BLOB NOT NULL,
    previous_secret_sealed BLOB,
    previous_expires_at INTEGER,
    enabled INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    disabled_at INTEGER
);

CREATE TABLE delivery_outbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_seq INTEGER NOT NULL REFERENCES events(seq),
    subscription_id TEXT NOT NULL REFERENCES webhook_subscriptions(id),
    state TEXT NOT NULL CHECK (state IN ('pending', 'delivered', 'dead', 'cancelled')),
    attempts INTEGER NOT NULL DEFAULT 0,
    next_attempt_at INTEGER NOT NULL,
    first_attempt_at INTEGER,
    last_error TEXT,
    UNIQUE (event_seq, subscription_id)
);
CREATE INDEX delivery_due ON delivery_outbox (state, next_attempt_at);
