-- A Discord sign-in is claimed by the first answer that arrives for it; only
-- that answer may take it, and any other answer for it is turned away.
ALTER TABLE discord_sign_ins ADD COLUMN claimed INTEGER NOT NULL DEFAULT 0;
