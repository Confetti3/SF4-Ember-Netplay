-- The game an organised match, lobby or tournament is for: 'usf4' (SF4
-- Ember) or 'sfv' (SFV Ember), as its create command named it
-- (`ember_protocol::matches::Game`); a lobby's or tournament's sets take its
-- game. A player's assignments name it, and the player record counts one
-- game's matches. Every row from before is USF4's.
--
-- Additive only: no key or index changes, and a row written without a game
-- is USF4's, so the binary from before 059 keeps working on a migrated
-- database (it writes and reads USF4's rows as it always did).
ALTER TABLE matches ADD COLUMN game TEXT NOT NULL DEFAULT 'usf4';
ALTER TABLE lobbies ADD COLUMN game TEXT NOT NULL DEFAULT 'usf4';
ALTER TABLE tournaments ADD COLUMN game TEXT NOT NULL DEFAULT 'usf4';
