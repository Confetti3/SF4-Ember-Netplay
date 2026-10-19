-- A set's replays (docs/design/TOURNAMENT_DESK.md, 4b): each game with a
-- confirmed result, as one of the set's players sent it, kept once by its
-- CRC, with the room binding's revision it was played under and the save
-- time from its record. The pack serves them in save-time order.
CREATE TABLE match_replays (
    match_id TEXT NOT NULL REFERENCES matches(id),
    crc TEXT NOT NULL,
    revision INTEGER NOT NULL,
    saved_at INTEGER NOT NULL,
    sent_by TEXT NOT NULL,
    replay BLOB NOT NULL,
    received_at INTEGER NOT NULL,
    PRIMARY KEY (match_id, crc)
);
