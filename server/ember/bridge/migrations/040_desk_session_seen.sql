-- Web desk sessions (routes::sign_in::desk) also end after two idle hours,
-- and the game lists them (`GET /v1/web-sessions`): when each started, when
-- it was last used, the site it signed in with and the browser's short
-- name. `noticed` is set once the game has told the organizer about it.
-- Sessions from before this have no last use and end at once.
ALTER TABLE desk_sessions ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE desk_sessions ADD COLUMN last_seen INTEGER NOT NULL DEFAULT 0;
ALTER TABLE desk_sessions ADD COLUMN site TEXT NOT NULL DEFAULT '';
ALTER TABLE desk_sessions ADD COLUMN agent TEXT NOT NULL DEFAULT '';
ALTER TABLE desk_sessions ADD COLUMN noticed INTEGER NOT NULL DEFAULT 0;
