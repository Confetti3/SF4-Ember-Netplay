-- Whether the room's creator is in it, as its room host last reported
-- (`details.creator_in`): NULL until a host that knows the field has said.
-- A room counts as its creator's one open room until it has opened and its
-- host says the creator is gone (routes/rooms/create.rs), so a creator whose
-- room others still play in can open another. A creator can then have two
-- open rooms, so the unique index gives way to a plain one; the check, in one
-- write transaction on the bridge's one connection, keeps the limit.
ALTER TABLE rooms ADD COLUMN creator_in INTEGER;
DROP INDEX rooms_one_open_per_creator;
CREATE INDEX rooms_open_by_creator ON rooms (creator_ember_id) WHERE closed_at IS NULL;
