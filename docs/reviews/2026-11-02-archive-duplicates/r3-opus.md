# Round 3 (Opus)

Round 2 F1-F6 fixed; the Discord gate is placed right (no finding).

- F1 P1: a set's copy of a file whose online row is still `duplicate` misses the lookup and hits the digest primary key; through `sets::backfill` it rolls back the whole maintenance pass, through the set route it is a 500. Fix: take the duplicate's row back for the set copy, then fold.
- F2 P2 (security): a set's copy folds every player's online copy, so a forged set copy deletes a stranger's stored game. Fix: carry the roster's Ember IDs on SetGame; fold only copies its players sent.
- F3 P3: "temporary" claim wrong since the set fold makes duplicates permanently; only the reading back of old copies is temporary.
- F4 P3: admit's ORDER BY lacks fold's digest tiebreak.
- F5 P3: tests/archive.rs 1,337 -> 1,726 lines; move the legacy harness out; stale `connect` doc line.
- F6 P3: Duplicates::attempt nested four deep.

VERDICT: CHANGES REQUIRED
