-- A Discord sign-in is claimed by the first answer that arrives for it; only
-- that answer may take it, and any other answer for it is turned away.
ALTER TABLE discord_sign_ins ADD COLUMN claimed INTEGER NOT NULL DEFAULT 0;

-- Connections where the player removed the link their Discord account's
-- sign-in approved. Lookups there do not link the account again. A new
-- sign-in replaces the account's row, and these go with it.
CREATE TABLE discord_withdrawals (
    user_id TEXT NOT NULL REFERENCES discord_accounts(user_id) ON DELETE CASCADE,
    connection_id TEXT NOT NULL,
    PRIMARY KEY (user_id, connection_id)
) WITHOUT ROWID;
