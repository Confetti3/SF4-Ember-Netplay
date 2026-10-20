-- Whether the bridge created an organizer's event on its site (routes::
-- organizer create), so the organizer may have it delete the site's event
-- too. Events picked from the site's own list are never deleted there.
ALTER TABLE organizer_events ADD COLUMN created_on_site INTEGER NOT NULL DEFAULT 0;
