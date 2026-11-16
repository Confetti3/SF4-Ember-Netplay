-- Every game's fighters (ember_protocol::levels::MAX_FIGHTERS): a fighter
-- index is 0 to 63, and each game's own roster (`rooms::roster`, 44 for
-- USF4, 45 for SFV) is checked where the bridge stores one: a counted game's
-- fighters (`skill::record_games`), a saved main (`routes::profiles::save`)
-- and a friends sync's main (`routes::friends::sync`). 060, 061 and 064 held
-- the fighters to USF4's 0 to 43, so SFV's 45th fighter could not be stored
-- and a game naming it rolled back its room's games. SQLite cannot change a
-- CHECK in place, so each table is copied into one with the wider check, as
-- 060 copied `rated_games`; nothing else of them changes.

CREATE TABLE fighter_levels_next (
    discord_user_id TEXT NOT NULL,
    fighter INTEGER NOT NULL CHECK (fighter BETWEEN 0 AND 63),
    games INTEGER NOT NULL,
    wins INTEGER NOT NULL CHECK (wins <= games),
    first_at INTEGER NOT NULL,
    played_at INTEGER NOT NULL,
    lower_wins INTEGER NOT NULL DEFAULT 0 CHECK (lower_wins BETWEEN 0 AND wins),
    PRIMARY KEY (discord_user_id, fighter)
) WITHOUT ROWID;
INSERT INTO fighter_levels_next (discord_user_id, fighter, games, wins, first_at, played_at, lower_wins)
    SELECT discord_user_id, fighter, games, wins, first_at, played_at, lower_wins FROM fighter_levels;
DROP TABLE fighter_levels;
ALTER TABLE fighter_levels_next RENAME TO fighter_levels;

CREATE TABLE social_accounts_next (
    discord_user_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    country TEXT,
    main_fighter INTEGER CHECK (main_fighter BETWEEN 0 AND 63),
    code TEXT NOT NULL UNIQUE,
    code_at INTEGER NOT NULL,
    appear_offline INTEGER NOT NULL DEFAULT 0,
    seen_at INTEGER NOT NULL,
    off INTEGER NOT NULL DEFAULT 0 CHECK (off IN (0, 1))
) WITHOUT ROWID;
INSERT INTO social_accounts_next (discord_user_id, name, country, main_fighter, code, code_at,
                                  appear_offline, seen_at, off)
    SELECT discord_user_id, name, country, main_fighter, code, code_at, appear_offline, seen_at, off
    FROM social_accounts;
DROP TABLE social_accounts;
ALTER TABLE social_accounts_next RENAME TO social_accounts;

CREATE TABLE player_cards_next (
    discord_user_id TEXT PRIMARY KEY,
    name TEXT NOT NULL COLLATE NOCASE,
    country TEXT,
    main_fighter INTEGER CHECK (main_fighter BETWEEN 0 AND 63),
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
INSERT INTO player_cards_next (discord_user_id, name, country, main_fighter, since, worn_title,
                               worn_icon, pins, device, link, region, looking, updated_at)
    SELECT discord_user_id, name, country, main_fighter, since, worn_title, worn_icon, pins,
           device, link, region, looking, updated_at
    FROM player_cards;
DROP TABLE player_cards;
ALTER TABLE player_cards_next RENAME TO player_cards;
CREATE INDEX player_cards_name ON player_cards (name COLLATE NOCASE, updated_at);
