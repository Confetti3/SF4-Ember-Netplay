-- Discord calls (docs/design/TOURNAMENT_DESK.md): an organizer's event can
-- name a Discord channel webhook, kept sealed with the bridge's encryption
-- key. When the desk creates a set's Ember match, or the organizer calls the
-- set again, the bridge posts the call there; with `post_results` it also
-- posts each finished set's score. `called_match` and `result_match` are the
-- match a set was last called or reported for, so a set is called once per
-- match and its result posted once.
ALTER TABLE organizer_events ADD COLUMN discord_webhook BLOB;
ALTER TABLE organizer_events ADD COLUMN post_results INTEGER NOT NULL DEFAULT 0;
ALTER TABLE desk_marks ADD COLUMN called_match TEXT;
ALTER TABLE desk_marks ADD COLUMN result_match TEXT;

-- The posts waiting for or done with Discord, sent by `delivery::calls`.
CREATE TABLE desk_posts (
    id INTEGER PRIMARY KEY,
    event_id TEXT NOT NULL REFERENCES organizer_events (id) ON DELETE CASCADE,
    set_id TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('call', 'result')),
    body TEXT NOT NULL,
    state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending', 'delivered', 'dead')),
    attempts INTEGER NOT NULL DEFAULT 0,
    next_attempt_at INTEGER NOT NULL,
    first_attempt_at INTEGER,
    last_error TEXT,
    created_at INTEGER NOT NULL
);
CREATE INDEX desk_posts_due ON desk_posts (state, next_attempt_at);
CREATE INDEX desk_posts_set ON desk_posts (event_id, set_id, id);
