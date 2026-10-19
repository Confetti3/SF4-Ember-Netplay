-- Organizers' own events (routes::organizer, docs/design/TOURNAMENT_DESK.md):
-- an event a signed-in organizer runs on the bridge's desk connection, by
-- the ID the bracket connector takes, with its round rules and holds as the
-- connector reads them. An event runs until the organizer stops it
-- (`ended_at`). One event runs once at a time, whoever starts it.
CREATE TABLE organizer_events (
    id TEXT PRIMARY KEY,
    ember_id TEXT NOT NULL,
    site TEXT NOT NULL,
    tournament TEXT NOT NULL,
    name TEXT NOT NULL,
    settings TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    ended_at INTEGER
);
CREATE INDEX organizer_events_ember ON organizer_events (ember_id, ended_at);
CREATE UNIQUE INDEX organizer_events_running ON organizer_events (site, tournament)
    WHERE ended_at IS NULL;
