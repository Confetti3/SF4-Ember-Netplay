-- Areas (docs/design/PUBLIC_ROOMS.md, "Where a room is"): where a player
-- plays from, their own claim, apart from the relay they connect through.
--
-- The area a public room's creator gave, one of the area codes
-- (ember_protocol::area::Area), or NULL when they gave none: a creator who hid
-- it, or a build from before areas. The listing shows it as the room's place;
-- `creator_region` stays the creator's relay, for routing and for 1.5.7
-- builds. A nullable column with no default only changes the table's
-- definition, so adding it is cheap and safe on the live table, and a bridge
-- from before it, put back, names its columns and leaves this one NULL.
ALTER TABLE rooms ADD COLUMN creator_area TEXT;

-- A card's region is the player's area from now on. Nightly builds before it
-- saved their relay's code there, which says how they connected, not where
-- they are: it reads as no region until they save again.
UPDATE player_cards SET region = NULL WHERE region IN ('use1', 'usw1', 'euc1', 'aps1');
