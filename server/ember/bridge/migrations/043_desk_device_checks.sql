-- Another device's code, checked before it is approved (routes::sign_in::desk):
-- what the browser that asked is (its browser and system, read from its
-- user agent; not the agent itself) and when, which the game shows; the
-- Ember ID whose game read the code; and the number the browser then shows,
-- which the organizer picks among the game's choices. A wrong pick ends the
-- code.
ALTER TABLE desk_devices ADD COLUMN device TEXT NOT NULL DEFAULT '';
ALTER TABLE desk_devices ADD COLUMN requested_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE desk_devices ADD COLUMN claimed_by TEXT;
ALTER TABLE desk_devices ADD COLUMN number INTEGER;
ALTER TABLE desk_devices ADD COLUMN choices TEXT;
