-- The archive listing's index (docs/design/REPLAY_ARCHIVE.md, "Listing"), so
-- a listing reads only the games it lists, whatever it filters on and
-- however large the archive grows.
--
-- Everything here is derived from `archive_games` (and a set's roster) by the
-- triggers below, so every write to the archive, wherever it is made, keeps
-- it: no code path has to remember to. The triggers rely on SQLite's default
-- `recursive_triggers = OFF`: a trigger's own UPDATE never fires itself, only
-- the triggers after it in the chain sides -> classes -> keys.
--
-- * `player_a` / `player_b`: the Ember ID that played each side, as far as
--   the archive knows it (`archive_side_players`).
-- * `class_a` / `class_b`: each side's class on the public rankings board,
--   as `archive_player_classes` holds it (`archive::classes::refresh`).
-- * `archive_keys`: one row per stored game and each thing a listing picks
--   games by (`archive_game_keys`), newest first within each key. It holds
--   a game's digest as its 32 bytes, half its hex, since there are a dozen
--   rows a game; blobs and lowercase hex sort alike.
-- * `archive_key_counts`: how many stored games each key has.
-- * `archive_kept`: how many games the archive holds in any state and their
--   bytes, which the admission's bound reads (`archive::refusal`).
--
-- The bridge before this one lists through `archive_games_listed` and
-- `archive_games_event`, so they stay: going back to it needs them.

ALTER TABLE archive_games ADD COLUMN player_a TEXT;
ALTER TABLE archive_games ADD COLUMN player_b TEXT;
ALTER TABLE archive_games ADD COLUMN class_a INTEGER;
ALTER TABLE archive_games ADD COLUMN class_b INTEGER;
CREATE INDEX archive_games_player_a ON archive_games (player_a) WHERE player_a IS NOT NULL;
CREATE INDEX archive_games_player_b ON archive_games (player_b) WHERE player_b IS NOT NULL;
CREATE INDEX archive_games_set ON archive_games (match_id) WHERE match_id IS NOT NULL;

-- The public class of every player the board shows, as the archive's games
-- carry it. `settled` is 0 while some of the player's games still carry
-- another class; a row whose `band` is NULL is a player who left the board,
-- removed once none of their games carries a class any more.
CREATE TABLE archive_player_classes (
    ember_id TEXT PRIMARY KEY,
    band INTEGER,
    settled INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
CREATE INDEX archive_player_classes_unsettled ON archive_player_classes (ember_id) WHERE settled = 0;

CREATE TABLE archive_keys (
    key TEXT NOT NULL,
    saved_at INTEGER NOT NULL,
    digest BLOB NOT NULL,
    PRIMARY KEY (key, saved_at, digest)
) WITHOUT ROWID;

CREATE TABLE archive_key_counts (
    key TEXT PRIMARY KEY,
    games INTEGER NOT NULL
) WITHOUT ROWID;

CREATE TABLE archive_kept (
    one INTEGER PRIMARY KEY CHECK (one = 1),
    games INTEGER NOT NULL,
    bytes INTEGER NOT NULL
);

-- Who played each side: a set's game by its roster as assigned now, an
-- online game by whoever sent it from that side or named that side later
-- (`archive::name_side`).
CREATE VIEW archive_side_players AS
SELECT g.digest,
    CASE WHEN g.match_id IS NOT NULL THEN
        (SELECT p.ember_id FROM match_participants p JOIN matches m ON m.id = p.match_id
         WHERE p.match_id = g.match_id AND p.assignment_generation = m.assignment_generation AND p.slot = 0)
    WHEN g.sent_side = 0 THEN g.sent_by ELSE g.named_by_a END AS player_a,
    CASE WHEN g.match_id IS NOT NULL THEN
        (SELECT p.ember_id FROM match_participants p JOIN matches m ON m.id = p.match_id
         WHERE p.match_id = g.match_id AND p.assignment_generation = m.assignment_generation AND p.slot = 1)
    WHEN g.sent_side = 1 THEN g.sent_by ELSE g.named_by_b END AS player_b
FROM archive_games g;

-- The keys of each stored game, each once:
--   *            every stored game
--   f:<id>       a fighter either side played
--   c:<code>     a country either side is from
--   n:<name>     a side's name, which a search looks through
--   o            an online game (no set); e:<title> a set's event
--   r:<round>    top8, pools, winners, losers, grand
--   k:<band>     a class either side has
--   t:<band>     the higher class of the two sides, 0 for none: the class order
CREATE VIEW archive_game_keys AS
SELECT digest, saved_at, '*' AS key FROM archive_games WHERE state = 'stored'
UNION ALL SELECT digest, saved_at, 'f:' || fighter_a FROM archive_games WHERE state = 'stored'
UNION ALL SELECT digest, saved_at, 'f:' || fighter_b FROM archive_games
    WHERE state = 'stored' AND fighter_b IS NOT fighter_a
UNION ALL SELECT digest, saved_at, 'c:' || country_a FROM archive_games
    WHERE state = 'stored' AND country_a IS NOT NULL
UNION ALL SELECT digest, saved_at, 'c:' || country_b FROM archive_games
    WHERE state = 'stored' AND country_b IS NOT NULL AND country_b IS NOT country_a
UNION ALL SELECT digest, saved_at, 'n:' || name_a FROM archive_games
    WHERE state = 'stored' AND name_a IS NOT NULL
UNION ALL SELECT digest, saved_at, 'n:' || name_b FROM archive_games
    WHERE state = 'stored' AND name_b IS NOT NULL AND name_b IS NOT name_a
UNION ALL SELECT digest, saved_at, 'o' FROM archive_games WHERE state = 'stored' AND match_id IS NULL
UNION ALL SELECT digest, saved_at, 'e:' || event_title FROM archive_games
    WHERE state = 'stored' AND event_title IS NOT NULL
UNION ALL SELECT digest, saved_at, 'r:top8' FROM archive_games WHERE state = 'stored' AND phase LIKE 'Top%'
UNION ALL SELECT digest, saved_at, 'r:pools' FROM archive_games
    WHERE state = 'stored' AND match_id IS NOT NULL AND COALESCE(phase, '') NOT LIKE 'Top%'
UNION ALL SELECT digest, saved_at, 'r:winners' FROM archive_games
    WHERE state = 'stored' AND round_label LIKE 'Winners%'
UNION ALL SELECT digest, saved_at, 'r:losers' FROM archive_games
    WHERE state = 'stored' AND round_label LIKE 'Losers%'
UNION ALL SELECT digest, saved_at, 'r:grand' FROM archive_games
    WHERE state = 'stored' AND round_label LIKE 'Grand%'
UNION ALL SELECT digest, saved_at, 'k:' || class_a FROM archive_games
    WHERE state = 'stored' AND class_a IS NOT NULL
UNION ALL SELECT digest, saved_at, 'k:' || class_b FROM archive_games
    WHERE state = 'stored' AND class_b IS NOT NULL AND class_b IS NOT class_a
UNION ALL SELECT digest, saved_at, 't:' || MAX(COALESCE(class_a, 0), COALESCE(class_b, 0)) FROM archive_games
    WHERE state = 'stored';

-- What the games kept so far have. No class yet: the first maintenance pass
-- gives them theirs.
UPDATE archive_games SET (player_a, player_b) =
    (SELECT s.player_a, s.player_b FROM archive_side_players s WHERE s.digest = archive_games.digest);
INSERT OR IGNORE INTO archive_keys (key, saved_at, digest) SELECT key, saved_at, unhex(digest) FROM archive_game_keys;
INSERT INTO archive_key_counts (key, games) SELECT key, COUNT(*) FROM archive_keys GROUP BY key;
INSERT INTO archive_kept (one, games, bytes)
    SELECT 1, COUNT(*), COALESCE(SUM(size), 0) FROM archive_games;

-- archive_kept: every row, in any state, as the admission's bound counts.
CREATE TRIGGER archive_kept_insert AFTER INSERT ON archive_games BEGIN
    UPDATE archive_kept SET games = games + 1, bytes = bytes + NEW.size;
END;
CREATE TRIGGER archive_kept_delete AFTER DELETE ON archive_games BEGIN
    UPDATE archive_kept SET games = games - 1, bytes = bytes - OLD.size;
END;

-- Sides: a new game, a change of who sent or named a side or of its set,
-- and a set's roster as it is assigned again.
CREATE TRIGGER archive_sides_insert AFTER INSERT ON archive_games BEGIN
    UPDATE archive_games SET (player_a, player_b) =
        (SELECT s.player_a, s.player_b FROM archive_side_players s WHERE s.digest = NEW.digest)
    WHERE digest = NEW.digest;
END;
CREATE TRIGGER archive_sides_update
AFTER UPDATE OF match_id, sent_by, sent_side, named_by_a, named_by_b ON archive_games
WHEN OLD.match_id IS NOT NEW.match_id OR OLD.sent_by IS NOT NEW.sent_by
    OR OLD.sent_side IS NOT NEW.sent_side OR OLD.named_by_a IS NOT NEW.named_by_a
    OR OLD.named_by_b IS NOT NEW.named_by_b
BEGIN
    UPDATE archive_games SET (player_a, player_b) =
        (SELECT s.player_a, s.player_b FROM archive_side_players s WHERE s.digest = NEW.digest)
    WHERE digest = NEW.digest;
END;
CREATE TRIGGER archive_sides_roster AFTER INSERT ON match_participants BEGIN
    UPDATE archive_games SET (player_a, player_b) =
        (SELECT s.player_a, s.player_b FROM archive_side_players s WHERE s.digest = archive_games.digest)
    WHERE match_id = NEW.match_id;
END;
CREATE TRIGGER archive_sides_generation AFTER UPDATE OF assignment_generation ON matches
WHEN OLD.assignment_generation IS NOT NEW.assignment_generation
BEGIN
    UPDATE archive_games SET (player_a, player_b) =
        (SELECT s.player_a, s.player_b FROM archive_side_players s WHERE s.digest = archive_games.digest)
    WHERE match_id = NEW.id;
END;

-- Classes: a side's player, as the board's classes stand in the archive.
CREATE TRIGGER archive_classes_players AFTER UPDATE OF player_a, player_b ON archive_games
WHEN OLD.player_a IS NOT NEW.player_a OR OLD.player_b IS NOT NEW.player_b
BEGIN
    UPDATE archive_games SET
        class_a = (SELECT band FROM archive_player_classes WHERE ember_id = NEW.player_a),
        class_b = (SELECT band FROM archive_player_classes WHERE ember_id = NEW.player_b)
    WHERE digest = NEW.digest;
END;

-- Keys: before a change to what they are made of, the game's keys as they
-- stand go; after it, its keys as they are now come in. The view says what
-- they are both times, so they are always the view's.
CREATE TRIGGER archive_keys_before_update
BEFORE UPDATE OF state, fighter_a, fighter_b, name_a, name_b, country_a, country_b,
    match_id, event_title, round_label, phase, class_a, class_b ON archive_games
WHEN OLD.state = 'stored' AND (OLD.state IS NOT NEW.state
    OR OLD.fighter_a IS NOT NEW.fighter_a OR OLD.fighter_b IS NOT NEW.fighter_b
    OR OLD.name_a IS NOT NEW.name_a OR OLD.name_b IS NOT NEW.name_b
    OR OLD.country_a IS NOT NEW.country_a OR OLD.country_b IS NOT NEW.country_b
    OR OLD.match_id IS NOT NEW.match_id OR OLD.event_title IS NOT NEW.event_title
    OR OLD.round_label IS NOT NEW.round_label OR OLD.phase IS NOT NEW.phase
    OR OLD.class_a IS NOT NEW.class_a OR OLD.class_b IS NOT NEW.class_b)
BEGIN
    DELETE FROM archive_keys WHERE (key, saved_at, digest) IN
        (SELECT key, saved_at, unhex(digest) FROM archive_game_keys WHERE digest = OLD.digest);
END;
CREATE TRIGGER archive_keys_after_update
AFTER UPDATE OF state, fighter_a, fighter_b, name_a, name_b, country_a, country_b,
    match_id, event_title, round_label, phase, class_a, class_b ON archive_games
WHEN NEW.state = 'stored' AND (OLD.state IS NOT NEW.state
    OR OLD.fighter_a IS NOT NEW.fighter_a OR OLD.fighter_b IS NOT NEW.fighter_b
    OR OLD.name_a IS NOT NEW.name_a OR OLD.name_b IS NOT NEW.name_b
    OR OLD.country_a IS NOT NEW.country_a OR OLD.country_b IS NOT NEW.country_b
    OR OLD.match_id IS NOT NEW.match_id OR OLD.event_title IS NOT NEW.event_title
    OR OLD.round_label IS NOT NEW.round_label OR OLD.phase IS NOT NEW.phase
    OR OLD.class_a IS NOT NEW.class_a OR OLD.class_b IS NOT NEW.class_b)
BEGIN
    INSERT OR IGNORE INTO archive_keys (key, saved_at, digest)
        SELECT key, saved_at, unhex(digest) FROM archive_game_keys WHERE digest = NEW.digest;
END;
CREATE TRIGGER archive_keys_insert AFTER INSERT ON archive_games WHEN NEW.state = 'stored' BEGIN
    INSERT OR IGNORE INTO archive_keys (key, saved_at, digest)
        SELECT key, saved_at, unhex(digest) FROM archive_game_keys WHERE digest = NEW.digest;
END;
CREATE TRIGGER archive_keys_delete BEFORE DELETE ON archive_games WHEN OLD.state = 'stored' BEGIN
    DELETE FROM archive_keys WHERE (key, saved_at, digest) IN
        (SELECT key, saved_at, unhex(digest) FROM archive_game_keys WHERE digest = OLD.digest);
END;

-- Counts: a key no game has any more goes, so a search looks only through
-- names some stored game carries.
CREATE TRIGGER archive_key_counts_insert AFTER INSERT ON archive_keys BEGIN
    INSERT INTO archive_key_counts (key, games) VALUES (NEW.key, 1)
        ON CONFLICT (key) DO UPDATE SET games = games + 1;
END;
CREATE TRIGGER archive_key_counts_delete AFTER DELETE ON archive_keys BEGIN
    UPDATE archive_key_counts SET games = games - 1 WHERE key = OLD.key;
    DELETE FROM archive_key_counts WHERE key = OLD.key AND games <= 0;
END;
