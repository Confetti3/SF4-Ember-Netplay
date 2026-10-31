1. **P2 — r2.1: Keeper enforcement does not protect the selection actually used to start the match.** [sf4e__SessionServer__Handlers.cxx:389](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/session/sf4e__SessionServer__Handlers.cxx:389)

   The new check drops a forbidden `PreBattleSetChara`, but it does not prevent the following Ready from succeeding. After an Arcade rotation changes the pair, `ForgetReseatedMatchData` clears both native selections to default Ryu. If the keeper then submits a different, otherwise-valid fighter or edition, this handler silently discards it; Ready and `BeginAuthorizedTable` never verify that the native selection matches `keptFighter`/`keptEdition`. Once the other player supplies valid settings and both ready, the keeper can therefore enter as default Ryu despite the authority retaining another fighter.

   **Fix:** Make an accepted selection for the current member/seat pairing a prerequisite for Ready, and validate the actual native selection against the keeper contract before starting. Return an actionable refusal when that prerequisite fails. Keep selection acceptance and the start gate behind one canonical contract. Add a transport regression covering rotation, a rejected keeper pick, and subsequent Ready messages; checking `FighterAllowed` alone does not cover this failure.

2. **P3 — r2.2: The second edition control still advertises an adjustment that the keeper lock silently ignores.** [FighterSelector.cxx:315](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/FighterSelector.cxx:315)

   With edition selection enabled, a keeper can open **Additional options**, where the edition row at line 244 remains enabled and says changes save immediately. The new action guard ignores Left/Right on that row, while the unconditional tail still reports `changed=true`. The board’s edition row correctly disables itself and explains the restriction, so the duplicated controls now disagree.

   **Fix:** Construct both edition rows from the same eligibility and explanation, including `fighterKept`. Report a change only when an adjustment occurs. Cover the Additional options opening in the keeper journey.

The source changes address r1.1 and r1.4–r1.7. The keeper state added for r1.2/r1.3 is serialized and cleared on the reviewed departure paths, but r2.1 leaves its enforcement incomplete.

**1,000-line rule:** No scoped file crosses from below 1,000 lines to above it. `src/sf4e/sf4e__NetplayRuntime__Room.cxx` was already over the threshold and grows from **1,015 to 1,018 lines**.

No files were edited, and no build or tests were run. The scoped `git diff --check` passed. These conclusions are based on source inspection, including the untracked test and shared version header.

VERDICT: CHANGES REQUIRED

