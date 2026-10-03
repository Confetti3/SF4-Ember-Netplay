-- A sign-in by a Discord account that is connected to another Ember ID waits
-- for the player to confirm the move in the browser. The account that signed
-- in is held with the sign-in until the player moves it, keeps it where it
-- is, or the sign-in lapses.
ALTER TABLE discord_sign_ins ADD COLUMN pending_user_id TEXT;
ALTER TABLE discord_sign_ins ADD COLUMN pending_username TEXT;
