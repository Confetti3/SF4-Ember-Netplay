-- Watching a bridge match (docs/design/MATCH_WATCH.md). Additive: every
-- existing row keeps today's behaviour, so deploying changes no live match.

-- Anyone holding the match link may watch (the organizer's choice).
ALTER TABLE provider_connections ADD COLUMN watch_link_holders INTEGER NOT NULL DEFAULT 0;

-- Ember IDs approved to watch every match of a connection.
CREATE TABLE connection_watch_staff (
    connection_id TEXT NOT NULL REFERENCES provider_connections(id),
    ember_id TEXT NOT NULL,
    added_at INTEGER NOT NULL,
    added_by TEXT NOT NULL,
    PRIMARY KEY (connection_id, ember_id)
);

-- One match's revocations.
CREATE TABLE match_watch_denials (
    match_id TEXT NOT NULL REFERENCES matches(id),
    ember_id TEXT NOT NULL,
    set_at INTEGER NOT NULL,
    set_by TEXT NOT NULL,
    PRIMARY KEY (match_id, ember_id)
);

-- Watchers who asked to watch, with the endpoint their room joins from.
CREATE TABLE match_watchers (
    match_id TEXT NOT NULL REFERENCES matches(id),
    ember_id TEXT NOT NULL,
    endpoint_id TEXT NOT NULL,
    helper_instance_id TEXT NOT NULL,
    build_id TEXT NOT NULL,
    first_seen_at INTEGER NOT NULL,
    last_seen_at INTEGER NOT NULL,
    PRIMARY KEY (match_id, ember_id)
);
CREATE INDEX match_watchers_endpoint ON match_watchers (match_id, endpoint_id);

-- Two watchers who asked for one endpoint, once per pair and endpoint, so the
-- organizer's audit log records a loss when it first happens and not on every
-- claim after it.
CREATE TABLE match_watch_conflicts (
    match_id TEXT NOT NULL REFERENCES matches(id),
    loser TEXT NOT NULL,
    winner TEXT NOT NULL,
    endpoint_id TEXT NOT NULL,
    PRIMARY KEY (match_id, loser, winner, endpoint_id)
);

-- The watcher set the current binding names, as a JSON list of
-- {"ember_id", "endpoint_id"} the bridge decodes into a typed list when it
-- reads the row. NULL means none (an empty set is never written), which is
-- every row today.
ALTER TABLE match_rooms ADD COLUMN bound_watchers TEXT;

-- The binding revision of the last fighter-endpoint change; prepares are
-- checked against it. Backfilled so live matches keep today's check exactly.
ALTER TABLE match_rooms ADD COLUMN fighters_revision INTEGER NOT NULL DEFAULT 0;
UPDATE match_rooms SET fighters_revision = binding_revision;

-- Whether a fighter's latest claim said its helper reads a binding that names
-- watchers (play::Claim::watch). A claim from a helper that does not say reads
-- false, so the bridge never names a watcher in that fighter's binding.
ALTER TABLE match_claims ADD COLUMN watch INTEGER NOT NULL DEFAULT 0;
