-- What a connection's disputed matches do (docs/design/INTEGRATION_PATHS.md):
-- wait for review, or restart. Recorded the first time the connection is seen
-- and never changed, so a connection's matches keep their policy. BluMint's
-- connections have always restarted.
ALTER TABLE provider_connections ADD COLUMN disputes TEXT NOT NULL DEFAULT 'review'
    CHECK (disputes IN ('review', 'restart'));
UPDATE provider_connections SET disputes = 'restart' WHERE kind = 'blumint';

-- Public rooms a connection opened for one of its players. NULL for a room a
-- player created from Ember. Only these rooms have events, on that
-- connection's stream.
ALTER TABLE rooms ADD COLUMN connection_id TEXT REFERENCES provider_connections(id);
-- Why a connection's room closed: `closed_by_connection` or `ended`.
ALTER TABLE rooms ADD COLUMN closed_reason TEXT;
CREATE INDEX rooms_connection ON rooms (connection_id, created_at) WHERE connection_id IS NOT NULL;
