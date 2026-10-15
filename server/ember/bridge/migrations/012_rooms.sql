-- Public rooms (docs/design/PUBLIC_ROOMS.md). The room supervisor is the source
-- of truth for which rooms are alive; these rows are the bridge's view of them,
-- refreshed by polling it.

-- A room is pending from the moment its creator is admitted to create it until
-- the supervisor reports it hosted: `invitation_sealed` and `region` are NULL.
-- `members`, `tables_playing` and the invitation are the last known values.
-- `opened_at` is when the supervisor first reported a member; until then only
-- the creator is given a ticket, and the room stays open to others afterwards
-- even if it empties.
-- `creator_address` is a keyed hash of the creator's network address, kept only
-- while the room is open.
CREATE TABLE rooms (
    room_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    capacity INTEGER NOT NULL,
    build_id TEXT NOT NULL,
    creator_ember_id TEXT NOT NULL,
    creator_address TEXT,
    created_at INTEGER NOT NULL,
    closed_at INTEGER,
    opened_at INTEGER,
    members INTEGER NOT NULL DEFAULT 0,
    tables_playing INTEGER NOT NULL DEFAULT 0,
    invitation_sealed BLOB,
    region TEXT
);

-- One open room per creator; the constraint, not a check, decides a race.
CREATE UNIQUE INDEX rooms_one_open_per_creator ON rooms (creator_ember_id) WHERE closed_at IS NULL;
CREATE INDEX rooms_open ON rooms (build_id) WHERE closed_at IS NULL;

-- An Ember ID the room host kicked, for the room's life.
CREATE TABLE room_bans (
    room_id TEXT NOT NULL REFERENCES rooms(room_id),
    ember_id TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (room_id, ember_id)
) WITHOUT ROWID;
