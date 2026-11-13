-- Counters behind the milestone awards (docs/design/AWARDS.md) that the games
-- and points alone cannot tell: each Discord account's current and best run
-- of counted wins, and the highest class its rating has ever shown. Both only
-- ever go up in the awards' eyes (`best_streak`, `peak_band`), so an award is
-- never taken away. Kept for good, one row an account.
CREATE TABLE award_counters (
    discord_user_id TEXT PRIMARY KEY,
    streak INTEGER NOT NULL DEFAULT 0,
    best_streak INTEGER NOT NULL DEFAULT 0,
    peak_band INTEGER NOT NULL DEFAULT 0 CHECK (peak_band BETWEEN 0 AND 10),
    CHECK (best_streak >= streak)
) WITHOUT ROWID;

-- Every account that has counted games starts with the best run its record
-- proves: one win is a run of one, and the games before this migration were
-- not kept one by one, so nothing longer can be known. Its peak class is the
-- class it shows now.
INSERT INTO award_counters (discord_user_id, streak, best_streak, peak_band)
    SELECT p.discord_user_id, 0, MIN(p.wins, 1),
           COALESCE((SELECT s.band FROM skill_ratings s
                     WHERE s.game = 'usf4' AND s.discord_user_id = p.discord_user_id), 0)
    FROM account_points p;
-- And every account with a class but no counted game.
INSERT OR IGNORE INTO award_counters (discord_user_id, streak, best_streak, peak_band)
    SELECT discord_user_id, 0, 0, band FROM skill_ratings
    WHERE game = 'usf4' AND band > 0;
