-- Whether a Discord account takes part in ranking (docs/design/SKILL_BADGE.md),
-- when that last changed (NULL for never; it changes at most once a day), and
-- how the ranking board lists it: the name and country its game sent. An
-- account without a row takes part and is not listed. An account that stops
-- taking part keeps its rating, untouched, and its listing is forgotten.
CREATE TABLE ranking_choices (
    discord_user_id TEXT PRIMARY KEY,
    ranked INTEGER NOT NULL,
    changed_at INTEGER,
    name TEXT,
    country TEXT
) WITHOUT ROWID;
