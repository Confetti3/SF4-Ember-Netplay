-- When a match nobody plays expires (docs/guides/INTEGRATIONS.md, "Matches
-- nobody plays"), in unix seconds, and how long a quiet match lives. NULL for
-- lobby and tournament sets, which end with their lobby or bracket, and for
-- matches that were finished before this existed. `expires_at` is the current
-- deadline: creation, a permit and every decided game set it to now plus
-- `expiry_secs`, in the same transaction. Matches still open now get a day
-- from their last change, so ones left over from testing expire on the next
-- sweep.
ALTER TABLE matches ADD COLUMN expires_at INTEGER;
ALTER TABLE matches ADD COLUMN expiry_secs INTEGER;
UPDATE matches SET expires_at = MAX(created_at, updated_at) + 86400, expiry_secs = 86400
 WHERE state NOT IN ('completed', 'cancelled', 'failed', 'expired')
   AND lobby_id IS NULL AND tournament_id IS NULL;
CREATE INDEX matches_expiry ON matches (expires_at) WHERE expires_at IS NOT NULL;
