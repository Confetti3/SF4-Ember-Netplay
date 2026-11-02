-- A public room's type as its room host reports it (`details.room_type`):
-- 0 Classic, 1 Arcade, 2 King of the hill. NULL until a host that knows the
-- field has said; rooms from before it, and from such hosts, are Classic.
ALTER TABLE rooms ADD COLUMN room_type INTEGER;
