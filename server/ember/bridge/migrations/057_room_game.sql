-- The game a public room is for: 'usf4' (SF4 Ember) or 'sfv' (SFV Ember), as
-- its creator asked when it was made (`CreateRoom::game`), and fixed for its
-- life. Listings, tickets and the skill rating keep the games apart. Rooms
-- from before the column are SF4 Ember's.
ALTER TABLE rooms ADD COLUMN game TEXT NOT NULL DEFAULT 'usf4';
