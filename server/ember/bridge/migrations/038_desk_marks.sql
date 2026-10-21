-- Desk actions (docs/design/TOURNAMENT_DESK.md): sets the organizer holds
-- from the desk, which the bracket connector leaves unplayed until they let
-- them go, and when the organizer last called each set.
CREATE TABLE desk_marks (
    event_id TEXT NOT NULL REFERENCES organizer_events (id) ON DELETE CASCADE,
    set_id TEXT NOT NULL,
    held INTEGER NOT NULL DEFAULT 0,
    called_at INTEGER,
    PRIMARY KEY (event_id, set_id)
) WITHOUT ROWID;
