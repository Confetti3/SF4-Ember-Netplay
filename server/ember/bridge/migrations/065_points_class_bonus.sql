-- The class bonus (docs/design/FIGHTER_LEVELS.md, "Points and tiers"): a
-- counted game is 1 point for each player, and the winner earns 1 more only
-- against an opponent of the same class or a higher one. Games and wins stay
-- exact, as the awards read them, so what is stored beside them is the wins
-- that earned no bonus: points are games + wins - lower_wins.
--
-- Every row starts at 0, so nobody's points change: every win so far keeps
-- the bonus it was counted with. A bridge from before this migration, put
-- back after it, adds games and wins and leaves these columns alone, so the
-- wins it counts keep their bonus too, as it served them.
ALTER TABLE fighter_levels
    ADD COLUMN lower_wins INTEGER NOT NULL DEFAULT 0 CHECK (lower_wins BETWEEN 0 AND wins);
ALTER TABLE account_points
    ADD COLUMN lower_wins INTEGER NOT NULL DEFAULT 0 CHECK (lower_wins BETWEEN 0 AND wins);
