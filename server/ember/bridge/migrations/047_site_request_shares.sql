-- What each caller spent of a site's day (routes::sign_in::budget): one
-- Ember ID, or the web desk's sign-ins together, takes at most half of it.
CREATE TABLE site_request_shares (
    site TEXT NOT NULL,
    day TEXT NOT NULL,
    caller TEXT NOT NULL,
    spent INTEGER NOT NULL,
    PRIMARY KEY (site, day, caller)
) WITHOUT ROWID;
