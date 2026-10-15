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

-- Before this version a removal was known only from the link's history: a
-- player's (or account holder's) removal since the account's latest sign-in.
-- Those become withdrawals, and those players' sign-ins in flight end, so
-- none finishing after the upgrade gives the consent back.
INSERT OR IGNORE INTO discord_withdrawals (user_id, connection_id)
SELECT d.user_id, l.connection_id
FROM links l JOIN discord_accounts d ON l.claim_id = 'discord:' || d.user_id AND l.ember_id = d.ember_id
WHERE l.approved_via = 'discord' AND l.revoked_by IN ('unlinked_by_player', 'unlinked_by_browser')
  AND l.revoked_at >= d.connected_at;
DELETE FROM discord_sign_ins WHERE ember_id IN (
    SELECT d.ember_id FROM discord_withdrawals w JOIN discord_accounts d ON d.user_id = w.user_id);
