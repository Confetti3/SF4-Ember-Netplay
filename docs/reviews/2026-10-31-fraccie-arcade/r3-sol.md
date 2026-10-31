1. **P2 — Ready failure leaves the prompt’s optimistic “starting” state latched.** [src/ui/ApplicationShell.cxx:598](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell.cxx:598)

   When the opponent is already ready, choosing `again` can make `UpdatePostMatch` set `p.starting=true` while the local request is still pending. If that request subsequently fails, the new notification handler clears only `p.chose`. `DrawPostMatch` still treats `p.starting` as chosen, displays “Starting the next game,” and disables Rematch until the start grace expires and the prompt closes.

   The ordering also allows this transition **on the failure frame itself**: `UpdatePostMatch` runs at line 581, before the failure handler clears the stale choice. The added journey leaves the opponent unready, so it misses both cases. This affects Classic and the newly exposed Arcade/King continuation path.

   **Fix:** Process the failure within the post-match state update before deriving transitions, and invalidate the failed request’s optimistic choice and associated start state when no next game has actually started. Add journeys with the opponent ready, covering failure both before and after `starting` becomes latched.

2. **P3 — A retained pair’s opponent departure is still presented as a rotation.** [src/ui/ApplicationShell__PostMatch.cxx:158](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__PostMatch.cxx:158)

   After a lone Arcade/King pair finishes its set and receives the ordinary Rematch row, let the opponent leave their cabinet while the local WIN prompt remains open. `theirs < 0` makes `rotates=true`, although this was a departure. The rotation block at lines 172–176 then overwrites the opponent-left title with “Waiting for a challenger.” If the completed-set record still matches, lines 186–189 also replace the departed player’s “Left” state with winner/loser rotation wording.

   **Fix:** Distinguish a cabinet rotation from a departure using the canonical cabinet/member queries. Preserve the opponent-left title and departed state when the opponent has left their cabinet; retain rotation presentation for fighters who remain at cabinets. Add a departure journey alongside the retained-pair and King-rotation cases.

The local-seat targeting fix is consistent across `ToggleReady` callers. Pending Unlimited results now preserve the ordinary Rematch, King lone-pair readiness agrees with the authority, and `KeepsSeat` continues to preserve cabinets on Back and timeout. No changed file crosses 1,000 lines.

Source review only: no files were edited, and no build or tests were run. `git diff --check` passes.

VERDICT: CHANGES REQUIRED