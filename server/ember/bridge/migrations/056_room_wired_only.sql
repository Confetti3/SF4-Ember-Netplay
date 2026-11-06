-- Whether only players whose PC is not on Wi-Fi take a seat in the room (1), as
-- its room host last reported (`details.wired_only`; a report without it is a
-- room without the rule, or a host that predates it). The creator sets it just
-- after the room opens and its host can change it. Rooms from before the
-- column admit everyone.
ALTER TABLE rooms ADD COLUMN wired_only INTEGER NOT NULL DEFAULT 0;
