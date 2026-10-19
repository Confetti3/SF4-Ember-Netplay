-- Organizer sign-ins (routes::sign_in, docs/design/TOURNAMENT_DESK.md): a
-- sign-in that asks the site for its organizer scopes keeps the tokens it
-- gets, sealed with the bridge's encryption key, for the account it
-- connects. While the bridge's page asks the player, the sealed tokens wait
-- on the sign-in. Discord offers no organizer sign-in; its table gets the
-- columns only so one query serves both.
ALTER TABLE site_sign_ins ADD COLUMN organizer INTEGER NOT NULL DEFAULT 0;
ALTER TABLE site_sign_ins ADD COLUMN pending_grant BLOB;
ALTER TABLE discord_sign_ins ADD COLUMN organizer INTEGER NOT NULL DEFAULT 0;
ALTER TABLE discord_sign_ins ADD COLUMN pending_grant BLOB;

-- The organizer tokens of a connected site account: access and refresh
-- token, scope and expiry sealed together, and when they were last fetched.
-- They go with the account: a disconnect, a move to another Ember ID or a
-- newer sign-in of the account removes them.
CREATE TABLE organizer_grants (
    site TEXT NOT NULL,
    user_id TEXT NOT NULL,
    sealed BLOB NOT NULL,
    refreshed_at INTEGER NOT NULL,
    PRIMARY KEY (site, user_id),
    FOREIGN KEY (site, user_id) REFERENCES site_accounts (site, user_id) ON DELETE CASCADE
) WITHOUT ROWID;
