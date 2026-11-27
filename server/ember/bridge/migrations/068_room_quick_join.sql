-- Who is in a public room, as the room host last reported it, for Quick join
-- (docs/design/QUICK_JOIN.md): the members' class bands and the players
-- waiting alone. Both NULL until a room host that reports them has done so,
-- and NULL again for any one the bridge found invalid or a report leaves out.
-- `classes` is one byte per member, the band 0 to 10, ascending. `waiting` is
-- one record per waiting player, in order: the band (0 to 10), the length of
-- the player's area code (0 for none, at most 8), then that many bytes of the
-- code. Both are written with the room's other details and read from the row.
-- A nullable column with no default only changes the table's definition, so
-- adding them is cheap and safe on the live table, and a bridge from before
-- it, put back, names its columns and leaves these NULL.
ALTER TABLE rooms ADD COLUMN classes BLOB;
ALTER TABLE rooms ADD COLUMN waiting BLOB;
