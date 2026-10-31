Found **2 P1 issues and 5 P2 issues** in the scoped changes.

1. **P1 — Shrinking can strand a spectator’s terminal receipt.** [src/session/RoomModelActions.cxx:30](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/session/RoomModelActions.cxx:30)

   `HasOutstandingTerminalReceipt()` checks only outstanding **fighters**, through `FightersOutstanding()`. A queued spectator can unqueue during a game, become an `endingWatcher`, and disappear from the table when `CloseLiveGeneration()` runs, while still owing its terminal ACK. Once the fighters acknowledge and leave, the table passes both shrink checks. After shrinking, that spectator’s `AcknowledgeTerminal` fails at `FindTable(action.table)` with `UnknownTable`; its outstanding receipt continues blocking Queue, Watch and Ready.

   **Fix:** require every terminal recipient to finish before removing a table, or allow receipt-scoped acknowledgments against retained table history independently of `TablesInPlay()`. Add the missing regression with an ending spectator whose ACK arrives after the fighters’ ACKs. The new test covers only fighter receipts.

2. **P1 — “Winner keeps fighter” is a selector restriction, not an authority invariant.** [src/sf4e/sf4e__Overlay.cxx:260](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__Overlay.cxx:260)

   The new rule reaches `FighterSelector`, but the host’s `HandleSetChara()` still accepts any otherwise valid fighter and edition while the seat is editable. A client sending a valid `PreBattleSetChara` can therefore change the winner’s fighter, and the server replaces `matchData.chara[side]`. The wire flag does not enforce the rule.

   **Fix:** retain the winning fighter and edition as authoritative state, enforce them before mutating match data, and expose that same restriction to the selector. Keep Ultra and appearance changes permitted. Test the server boundary directly: reject a held winner’s fighter/edition change and accept permitted customization. Do not rely on an older or modified client implementing the new UI restriction.

3. **P2 — Historical `lastSet` data incorrectly reactivates the fighter lock after leaving.** [src/session/RoomQueries.hxx:237](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/session/RoomQueries.hxx:237)

   `KeepsFighter()` treats “won the last set recorded here” as “has continuously retained this seat since winning.” Those are different states. With two players, A can win at table 0, stand up, and sit again opposite B at table 0. `RemoveFromTable()` preserves `lastSet`, and `JoinTable()` preserves it too, so A is immediately locked again without winning another set. This contradicts the documented release condition, “until they … leave it.”

   **Fix:** model the active restriction’s lifetime separately from historical results, clearing it when the winner leaves that table. Share this state with the authoritative enforcement above. Add leave/reseat and table-reuse cases; keep `lastSet` available for post-match presentation.

4. **P2 — The authority does not enforce the room-type restriction on table-count changes.** [src/session/RoomModelActions.cxx:28](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/session/RoomModelActions.cxx:28)

   The UI explicitly excludes public and tournament rooms, but `ApplySetTableCount()` rejects only Arcade rooms. The acting host of a server-owned Classic room—or a bound fighter holding host powers—can submit `SetTableCount` directly and change the count. The public/tournament policy currently exists only in row visibility.

   **Fix:** put eligibility in a shared room-policy query and enforce it in the authority, including `serverOwned` and active tournament bindings. Use that query for the settings rows as well. Add authority tests proving those room types reject the action without changing revision or count.

5. **P2 — A rejected submission permanently consumes the automatic profile read.** [src/ui/RankingsPanel.cxx:144](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/RankingsPanel.cxx:144)

   `profileRead_` is recorded before `submit()`, and its return value is ignored. If the command queue rejects the submission, no standing request was dispatched, but every subsequent frame treats it as already attempted. The profile and Online row remain without a rank until the player opens Rankings.

   **Fix:** record the key only after successful submission, or after finding a matching loaded standing. Preserve the deliberate no-retry policy for an actual failed network read. Test one rejected submission followed by an accepted submission, followed by no further automatic reads.

6. **P2 — The HUD addition duplicates the existing badge renderer.** [src/ui/NameMarks.cxx:132](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/NameMarks.cxx:132)

   `DrawBadge()` repeats `DrawNameMarks()`’s emblem drawing, faded opacity, pill fill/border, text measurement and centering. `BadgeWidth()` also repeats the existing emblem/pill measurement logic. This creates two implementations of the same mark; a future change to badge appearance or fallback behavior must now be applied consistently in both.

   **Fix:** extract one internal mark measurement/drawing implementation with explicit font and dimensions. Let room name marks add their flag, spacing and tooltip around it, and let the HUD use it directly. This deletes duplicate rendering logic while preserving the two layouts.

7. **P2 — `VersionLine` introduces a second version parser that already disagrees with the canonical parser.** [src/ui/VersionLine.hxx:16](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/VersionLine.hxx:16)

   The updater already has `ParseVersion()` and `ClassifyReleaseKind()`, but this UI helper independently parses the suffix. For example, `1.5.0-Nightly20261031` is classified as Nightly by the canonical parser, which lowercases the word, and as Beta by `VersionLine`. The comment promising the updater’s classification is therefore false.

   **Fix:** move the version parsing/classification into a common module used by both updater and UI. Keep `VersionLine` responsible for formatting the resulting kind and label. This removes the duplicate policy instead of adding another normalization patch.

**File-size check:** no scoped file crosses from below 1,000 lines to above it. The only changed C++ file already above that threshold is `src/sf4e/sf4e__NetplayRuntime__Room.cxx`, growing from **1,015 to 1,018 lines**. Locale catalogs were already above the threshold.

Reviewed the 70 tracked changed files and the untracked `room_table_count_test.cxx`, with the requested exclusions. The seat-first routing retains the selected-table fallback for unseated members and spectators. The automatic standing read sends no listing, and the listing path remains gated on visiting Rankings. No files were edited; no build or tests were run. The scoped `git diff --check` passed.

VERDICT: CHANGES REQUIRED