1. **P2 — A delayed Ready refusal can still close the prompt and automatically surrender a Classic seat.** [src/ui/ApplicationShell__PostMatch.cxx:104](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__PostMatch.cxx:104)

   **Failure scenario:** Choose Rematch with the opponent’s choice already present, so `p.starting` becomes true. Let the battle finish and the runtime reject the held Ready while the table’s generation remains unchanged. The new handler extends `p.until` and clears `starting`, but the close condition at line 128 immediately succeeds on `!Fighting(v.session.match) && v.atMainMenu`. The grace period therefore provides no recovery opportunity. In Classic after a completed set, line 129 treats the cleared choice as no answer and schedules `LeaveSeat`, despite the player having explicitly chosen to continue.

   A refusal before `starting` also closes immediately if the original ten-second deadline has expired, because that case receives no extension.

   **Fix:** Give a refused pending continuation a retry window that survives both the expired original deadline and the ordinary game-over/no-answer transition. Preserve genuine next-game closure, and do not translate the refused choice into automatic seat abandonment. Add Classic coverage with refusal after battle exit, plus refusal after the original deadline. The new cabinet tests inject refusal promptly while the session remains `Playing`, so they miss both paths.

2. **P3 — Departure detection still fails when the departure moves the local player to another table.** [src/ui/ApplicationShell__PostMatch.cxx:169](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__PostMatch.cxx:169)

   **Failure scenario:** A retained Arcade/King pair finishes its set; a third player then sits and waits alone. While the local WIN prompt remains open, the original opponent leaves their cabinet. `ArcadeVacated()` moves the remaining local player onto the third player’s waiting table. Consequently, both `mine` and `theirs` are negative at the prompt’s original table.

   The unconditional `mine < 0` arm still sets `rotates=true`, bypassing the new opponent-cabinet check. With the completed-set record intact, lines 197–200 overwrite the departed opponent’s **Left** state with **stays/waits**. The rotation title also replaces the opponent-left title. The new departure test keeps the local player at the original table, so it exercises only the guarded `theirs < 0` arm.

   **Fix:** Determine the original opponent’s departure independently of the local player’s current table. Give that departure precedence over rotation presentation, including the opponent’s state word. Extend the journey with a waiting third player and the authority’s resulting local-table move.

The local-seat targeting fix, pending Unlimited-result handling, King lone-pair continuation, and cabinet preservation on Back/timeout are otherwise consistent with the surrounding code. No changed file crosses 1,000 lines. `git diff --check` passes. This was a source review; no files were edited, and no build or tests were run.

VERDICT: CHANGES REQUIRED
