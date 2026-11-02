# Round 4 (Opus)

Round 3 F1-F6 fixed (upsert revival columns and lease interplay checked).

- F1 P1 (security): fold_kept_before called fold(game, None), folding strangers' copies into a set's copy after the roster-limited admit fold had left them. Fix: fold reads the kept set copy's roster itself; drop the parameter and SetGame.players.
- F2 P3: Duplicates record's retry UPDATE had no state guard, so a pre-revival Remove lease could move a revived set copy's schedule. Fix: AND state = the leased state.
- F3 P3: design doc still said "every other player's".

VERDICT: CHANGES REQUIRED
