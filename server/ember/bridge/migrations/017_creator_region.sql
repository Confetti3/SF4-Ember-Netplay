-- The region a room's creator reported when they created it, one of the known
-- relay region codes, or NULL when they gave none. The listing shows it in
-- place of `region`, which stays the room host's own and, being set only once
-- the supervisor reports the room hosted, the sign that it is.
ALTER TABLE rooms ADD COLUMN creator_region TEXT;
