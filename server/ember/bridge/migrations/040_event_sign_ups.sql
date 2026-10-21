-- Sign-ups through Ember (routes::sign_ups, docs/design/TOURNAMENT_DESK.md):
-- an organizer opens an event created on a site that takes participants,
-- and players with that site's account connected sign up from Ember. Each
-- sign-up is added to the site's event as a participant under their site
-- username, so the bracket connector finds them as it finds anyone else.
ALTER TABLE organizer_events ADD COLUMN sign_ups INTEGER NOT NULL DEFAULT 0;

-- `participant` is the site's ID for the player in the event, once added.
CREATE TABLE event_sign_ups (
    event_id TEXT NOT NULL REFERENCES organizer_events (id) ON DELETE CASCADE,
    ember_id TEXT NOT NULL,
    username TEXT NOT NULL,
    participant TEXT,
    signed_up_at INTEGER NOT NULL,
    PRIMARY KEY (event_id, ember_id)
) WITHOUT ROWID;
