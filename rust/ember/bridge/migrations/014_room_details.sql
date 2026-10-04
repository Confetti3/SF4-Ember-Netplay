-- What the room host last said about its room for the listing
-- (docs/design/PUBLIC_ROOMS.md): the moderator's name, the members' main
-- characters, whether the room is locked and the first table's set format and
-- rotation. All NULL until a room host that reports them has done so, and
-- NULL again for any one the bridge found invalid. `fighters` is one byte per
-- member, moderator first, 255 for none. `name` and `capacity` of the room
-- follow the host too, so they are updated in place.
ALTER TABLE rooms ADD COLUMN host_name TEXT;
ALTER TABLE rooms ADD COLUMN fighters BLOB;
ALTER TABLE rooms ADD COLUMN locked INTEGER;
ALTER TABLE rooms ADD COLUMN set_format INTEGER;
ALTER TABLE rooms ADD COLUMN rotation INTEGER;
