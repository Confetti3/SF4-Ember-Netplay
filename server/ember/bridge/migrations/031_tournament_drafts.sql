-- Tournament drafts (docs/design/TOURNAMENT_DESK.md, 4a): an organizer's
-- event starts as a draft with only a title. It gets its site's event (one
-- the site lists for them, or one the bridge created there with their
-- grant), its start time, settings and links, and runs once it is marked
-- ready and its start time has come. The events kept from 030 are ready and
-- have no start time, so they run as before.
CREATE TABLE organizer_events_031 (
    id TEXT PRIMARY KEY,
    ember_id TEXT NOT NULL,
    title TEXT NOT NULL,
    status TEXT NOT NULL CHECK (status IN ('draft', 'ready')),
    starts_at INTEGER,
    site TEXT,
    tournament TEXT,
    name TEXT,
    settings TEXT NOT NULL,
    links TEXT NOT NULL DEFAULT '[]',
    created_at INTEGER NOT NULL,
    ended_at INTEGER,
    CHECK (status = 'draft' OR tournament IS NOT NULL)
);
INSERT INTO organizer_events_031 (id, ember_id, title, status, site, tournament, name, settings,
    created_at, ended_at)
    SELECT id, ember_id, name, 'ready', site, tournament, name, settings, created_at, ended_at
    FROM organizer_events;
DROP TABLE organizer_events;
ALTER TABLE organizer_events_031 RENAME TO organizer_events;
CREATE INDEX organizer_events_ember ON organizer_events (ember_id, ended_at);
CREATE UNIQUE INDEX organizer_events_running ON organizer_events (site, tournament)
    WHERE ended_at IS NULL AND tournament IS NOT NULL;
