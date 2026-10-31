No remaining P1 / P2 / P3 findings in the scoped working-tree changes.

- **Ready targeting is consistent.** `ToggleReady` uses the local player’s resolved table and seat for description, Unready, deciding Rematch, and refusal re-check. Its other callers do not require focus-based targeting.
- **Retained pairs and rotations are distinguished correctly.** Lone Arcade and King pairs receive ordinary Rematch, including before an Unlimited result arrives. King’s continuation matches the authority’s existing lone-pair behavior. A departed opponent retains **Left** when the local player is subsequently paired elsewhere.
- **Refusal handling preserves the seat.** Failure processing precedes transition derivation and clears both `chose` and `starting` while the generation is unchanged. Sticky `answered` prevents a submitted continuation that later fails from becoming automatic Classic seat abandonment. Cabinet preservation through `KeepsSeat` remains intact.
- **The structure is proportionate.** The fixes remain within the existing seating and prompt boundaries. No changed file crosses 1,000 lines, and I found no structural regression warranting decomposition.

The journey additions cover focus-independent Ready, pending results, refusal before and after optimistic starting, Classic seat preservation at game end, and departure followed by local re-pairing. These were inspected as source; no build or tests were run. `git diff --check` passes. No files were edited.

VERDICT: APPROVE