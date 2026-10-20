-- Private match messages (delivery::dms): when the desk calls a set, each
-- player whose Ember ID has a Discord account connected is sent the match
-- link in a direct message by the bridge's Discord bot. A player without one
-- is kept as `no_discord`, so the desk can say who was not reached.
CREATE TABLE desk_dms (
    id INTEGER PRIMARY KEY,
    event_id TEXT NOT NULL REFERENCES organizer_events (id) ON DELETE CASCADE,
    set_id TEXT NOT NULL,
    slot INTEGER NOT NULL CHECK (slot IN (0, 1)),
    user_id TEXT,
    body TEXT NOT NULL,
    state TEXT NOT NULL DEFAULT 'pending'
        CHECK (state IN ('pending', 'delivered', 'dead', 'no_discord')),
    attempts INTEGER NOT NULL DEFAULT 0,
    next_attempt_at INTEGER NOT NULL,
    first_attempt_at INTEGER,
    last_error TEXT,
    created_at INTEGER NOT NULL
);
CREATE INDEX desk_dms_due ON desk_dms (state, next_attempt_at);
CREATE INDEX desk_dms_set ON desk_dms (event_id, set_id, id);
