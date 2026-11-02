-- Who named a side of an online archive game (docs/design/REPLAY_ARCHIVE.md,
-- "Who says what"). `sent_side` is the side the first sender said was
-- theirs, as `sent_by` is the first sender; a later copy from another
-- account may name the other side, and `named_by_a` / `named_by_b` keep
-- which account did, so a name stays attributable. Games stored before this
-- have no `sent_side`; where only one side carries a country or a name it is
-- taken from that.
ALTER TABLE archive_games ADD COLUMN sent_side INTEGER;
ALTER TABLE archive_games ADD COLUMN named_by_a TEXT;
ALTER TABLE archive_games ADD COLUMN named_by_b TEXT;
UPDATE archive_games SET sent_side = CASE
    WHEN country_a IS NOT NULL AND country_b IS NULL THEN 0
    WHEN country_b IS NOT NULL AND country_a IS NULL THEN 1
    WHEN name_a IS NOT NULL AND name_b IS NULL THEN 0
    WHEN name_b IS NOT NULL AND name_a IS NULL THEN 1
END WHERE match_id IS NULL AND sent_side IS NULL;
