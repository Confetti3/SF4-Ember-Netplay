1. **P2 — An intact pair still loses Rematch while an Unlimited game’s result is pending.** [src/ui/ApplicationShell__PostMatch.cxx:157](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__PostMatch.cxx:157)

   With `SetFormat::Unlimited`, `MidSet()` returns false even before `lastSet.generation` matches the prompt’s generation. Consequently, `cabinets=true`, `deciding=false`, and `stays=false` classify the unchanged pair as rotating. The prompt removes `again` and says someone is next, despite both original fighters remaining seated.

   For an Unlimited Arcade game, this can last throughout the prompt: [RematchInPlaceOffered](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__NetplayRuntime__Status.cxx:413) allows continuation, and [native-result publication](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__Game__Battle__System__NativeResult.cxx:107) holds the result until continuation or battle exit. Requiring that result before exposing the continuation choice creates a circular dependency. The new journeys prepopulate `lastSet.generation`, so they bypass this state.

   **Fix:** Treat a pending result separately from confirmed rotation. An unmatched `lastSet.generation` must not establish that the pair rotated. Simplify the classification around whether the original pair remains seated, with `deciding` selecting the voting controls. Add an Unlimited lone-pair journey that opens the prompt before the result arrives, checks the ordinary Rematch and neutral wording, then delivers the result.

2. **P2 — The newly exposed ordinary Rematch path permanently records a refused submission as chosen.** [src/ui/ApplicationShell__PostMatch.cxx:293](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__PostMatch.cxx:293)

   The retained-pair fix now routes lone Arcade and King pairs through `again`. That handler sets `postMatch_->chose=true` before calling `Send()` and ignores its return value. If submission returns false, the next frame still disables Rematch and displays the local player as having chosen, although no request was queued. The player cannot retry from the prompt.

   Asynchronous refusal has the same problem: `FailReady()` clears the runtime intent and publishes `readyFailureSequence`, but the shell only displays the error. It never clears `p.chose`; that latch otherwise clears only after observing committed readiness. The new journey checks only successful submission.

   **Fix:** Set `chose` only when `Send()` succeeds, and clear the outstanding optimistic choice when its Ready fails asynchronously. Preserve the runtime’s held/sent state while a request remains active. Add retained-pair checks for rejected submission and a subsequent Ready-failure notification, verifying that dismissal leaves Rematch retryable.

The **round 1 P1 targeting issue is fixed**: description, Unready, deciding Rematch, and refusal re-check consistently use the local place. The other `ToggleReady` callers do not require focus-based targeting. King’s lone-pair continuation agrees with the authority’s existing behavior: the pair stays together and readies again. `KeepsSeat` continues to preserve cabinets for Back and timeout in both room types.

No changed file crosses 1,000 lines, and `git diff --check` passes. This was a source review; no files were edited, and no build or tests were run.

VERDICT: CHANGES REQUIRED