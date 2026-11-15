-- What a Discord account's card shows anyone (docs/design/PROFILE.md): the
-- name, flag and main its game last sent, when it came to Ember, the award
-- families it wears and shows (family ids: the viewer's catalog works out
-- each family's level from the card's counters), and how it plays: device,
-- connection, region and what it is looking for, as small numbers. Written
-- by `PUT /v1/cards/me`, and the name, flag and main also by a friends sync
-- (1.6.0 games). One row per account, for good.
CREATE TABLE player_cards (
    discord_user_id TEXT PRIMARY KEY,
    name TEXT NOT NULL COLLATE NOCASE,
    country TEXT,
    main_fighter INTEGER CHECK (main_fighter BETWEEN 0 AND 43),
    since INTEGER NOT NULL,
    worn_title TEXT,
    worn_icon TEXT,
    pins TEXT NOT NULL DEFAULT '',
    device INTEGER NOT NULL DEFAULT 0 CHECK (device BETWEEN 0 AND 4),
    link INTEGER NOT NULL DEFAULT 0 CHECK (link BETWEEN 0 AND 2),
    region TEXT,
    looking INTEGER NOT NULL DEFAULT 0 CHECK (looking BETWEEN 0 AND 63),
    updated_at INTEGER NOT NULL
) WITHOUT ROWID;
-- Find a player: exact and leading matches of a name, any case.
CREATE INDEX player_cards_name ON player_cards (name COLLATE NOCASE, updated_at);

-- Every account friends or rankings already know, with what they sent last.
INSERT INTO player_cards (discord_user_id, name, country, main_fighter, since, updated_at)
SELECT s.discord_user_id, s.name, s.country, s.main_fighter,
    COALESCE((SELECT d.connected_at FROM discord_accounts d WHERE d.user_id = s.discord_user_id), s.seen_at),
    s.seen_at
FROM social_accounts s;
INSERT OR IGNORE INTO player_cards (discord_user_id, name, country, since, updated_at)
SELECT r.discord_user_id, r.name, r.country,
    COALESCE((SELECT d.connected_at FROM discord_accounts d WHERE d.user_id = r.discord_user_id), CAST(strftime('%s','now') AS INTEGER)),
    COALESCE(r.changed_at, CAST(strftime('%s','now') AS INTEGER))
FROM ranking_choices r WHERE r.game = 'usf4' AND r.name IS NOT NULL;
