-- The Ember ID a Discord account was connected to when the browser page asked
-- to connect or move it, NULL when it was connected to none. The player's
-- answer applies only while that is still so: a page left open while the
-- account was connected or moved elsewhere changes nothing.
ALTER TABLE discord_sign_ins ADD COLUMN pending_owner TEXT;
