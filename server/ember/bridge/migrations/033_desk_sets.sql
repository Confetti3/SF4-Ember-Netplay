-- Desk sets (docs/design/TOURNAMENT_DESK.md): the bracket connector sends
-- each running desk event's sets after every read; the bridge keeps the
-- latest, adds each Ember match's score and state, and serves it to the
-- organizer and, when they make the event public, to anyone.
ALTER TABLE organizer_events ADD COLUMN sets TEXT;
ALTER TABLE organizer_events ADD COLUMN sets_at INTEGER;
ALTER TABLE organizer_events ADD COLUMN public INTEGER NOT NULL DEFAULT 0;
