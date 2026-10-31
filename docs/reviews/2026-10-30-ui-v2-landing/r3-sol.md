Reviewed `fcd33f73..2e5493db`: 11 commits, 180 changed files. Four findings remain. The branch substantially improves the architecture, but the room-settings policy and shared-row rendering contracts are still incomplete.

1. **[P2] The room-edit policy does not govern the pointer paths that change the same settings.**

   **[ApplicationShell__Settings.cxx:SettingsScreens::RoomLayoutPicture](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__Settings.cxx:337)** → **[RoomPanelColumn.cxx:RoomGrips](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/RoomPanelColumn.cxx:31)**; also `RoomActions::RoomAction` and `ApplicationShell::DrawScreen`.

   `CanEditRoomSettings()` correctly gates the seven layout rows, the watched-picture rows, the PictureSpot map and automatic saving. However, `RoomLayoutPicture` discards its `ShellView` and draws active grips unconditionally. Those grips write `roomColumn` and `roomSplit` even when the corresponding rows are disabled. The board’s Hide/Show and picture choices, and the floating picture’s drag/resize path, likewise bypass the policy.

   For a joined room outside the main menu, the pointer can therefore change preferences that the pad cannot change and `MaySave` cannot submit yet. This preserves the disagreement identified by room-audit finding 16, whose requested rule explicitly covered **rows, grips and saving**.

   **Remedy:** pass the capability into the shared pointer controls and enforce it before preference mutation. Use the same capability for the board’s layout choices. Keep the preview visible when editing is unavailable. Add a journey with `CanEditRoomSettings()==false` that checks both row input and dragging, then checks that both work when it becomes true.

2. **[P2] Shared save-recovery actions still disappear inside custom board renderers.**

   **[ApplicationShell.cxx:ApplicationShell::DrawScreen](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell.cxx:733)** and **[ApplicationShell__Settings.cxx:SettingsScreens::DrawBoard](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__Settings.cxx:548)**.

   The shell appends `retry-save` to the final entries, but a closed Settings tab renders only its panel heads and the selected panel’s preview. It never renders or places the retry row. With geometry navigation, that unplaced row is also unreachable from the rail. The new null-panel handling correctly stops showing panel zero for a non-panel focus, but supplies no replacement presentation for the action.

   The awards board has the same boundary problem: `DrawAwardsBoard` skips every entry that does not resolve to an award. Separately, the retry row’s enablement still uses `canEditPreferences`, so a failed scoped room-layout save cannot be retried while joined, even when `MaySave` permits it.

   **Remedy:** give shell-owned actions an explicit footer that is rendered independently of each screen’s specialized body, and derive retry eligibility from the canonical save rule. This removes the need for individual boards to remember Retry saving. Extend `RetrySaveOnPanelScreens` to cover a closed Settings tab, awards, and a scoped room-settings failure; assert rendered bounds and navigation reachability as well as dispatch.

3. **[P2] The controller test file crosses the explicit 1,000-line boundary.**

   **[controller_navigation_test.cxx:DialogContract / HintPresses / DialogRepeat](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/tests/controller_navigation_test.cxx:846)**.

   This file grows from **892 to 1,032 lines**. It now combines native controller-memory fixtures, training-input lifecycle tests, navigation geometry, rendered dialogs, hint interactions and profile-record checks. There is a clear responsibility boundary, so the threshold crossing has no compelling structural justification.

   **Remedy:** decompose this first. Move the rendered menu/dialog tests and their headless fixtures into a focused test unit, retaining their execution through the existing target or a separately registered target. This is also a presumptive blocker under `.github/CONTRIBUTING.md:66`.

4. **[P3] Replacing a Settings panel still leaves its parent rail focused on the first panel.**

   **[ApplicationShell__Settings.cxx:SettingsScreens::DrawBoard](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__Settings.cxx:565)** and **[ApplicationShell.cxx:DrawScreen, Shortcut branch](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell.cxx:809)**.

   Open Match HUD from the Interface rail, click Room layout’s head, then press Back. `Replace` correctly replaces the child screen, but nothing updates the parent tab’s saved focus. Back therefore returns to Match HUD’s head. This is the original menu-audit finding 15; centralizing stack replacement did not resolve the parent-focus half of that transition.

   **Remedy:** make opening or replacing a Settings panel update both the parent head focus and the child screen as one transition. Cover rail-click replacement followed by Back.

The reported round 2 remedies otherwise check out:

| Item | Round 3 disposition |
|---|---|
| Sol 1: tab replacement | `Replace` is used by the shell and Training; the reader/tab regression test is present. Parent rail focus remains finding 4. |
| Sol 2: hint clicks | Resolved. `hintPress_` enters the normal input pipeline; dialog ownership suppresses screen shortcuts, tab hints and the bell. |
| Sol 3 / Opus 1: room-settings eligibility | Fixed for the enumerated rows, SpotMap, button-layout gate and save gate; pointer paths remain finding 1. |
| Sol 4: deciding Arcade prompt | Resolved in source. Character select is a navigable row; layout uses the actual row count; the stick journey is present. |
| Sol smaller items | Shared `DirectionRepeat`, scoped draft wiping and the common toast tail are implemented. New embedded-screen toasts cannot acquire menu focus. |
| Opus 2: panel IDs | Resolved. Stable IDs replace positional parsing, and non-panel focus selects no panel. Shared action rendering remains finding 2. |
| Opus 3: screen ownership | Public rooms, rankings and replays now supply their presentation through `ScreenPanel`. **I accept retaining the remaining `UpdateStatus` lines this round:** their ordered overrides express deliberate precedence, and moving them wholesale into the existing callback would change it. |
| Opus 4: combined LP/HP button | Resolved. Input and glyph resolution both exclude the combined button. |

The production decomposition is meaningful: `ApplicationShell.cxx` shrinks **1,195 → 834 lines**, and `RoomPanel.cxx` **1,355 → 725**. I found no additional blocker in the shared Arcade/King rotation, JSON serialization, UTF-8 consolidation or typed row parsing. All 14 committed locale catalogues have matching key sets; none of the 80 removed English keys retains an exact literal reference under `src`.

Validation used existing binaries only: **20 selected executables passed**, and headless UiRender passed **4,365 frames at 1920×1080/100%**. CTest could not create its log. Direct ShellJourney and IdentityJourney runs were blocked by temporary-directory permissions; SettingsStore exited with **`0xC0000409`** without diagnostics. The navigation binary contains the new hint-test assertions, but the provenance manifest still names an older nightly revision, so these results do not establish a fresh build of this HEAD. No build, edits, deployment or gameplay validation was performed; working-tree changes were excluded.

VERDICT: CHANGES REQUIRED

