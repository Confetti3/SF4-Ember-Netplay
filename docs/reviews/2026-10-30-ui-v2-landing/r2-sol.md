Changes are still required at **039e5cab4**. The round-one restructuring is substantially complete, but four input and capability problems remain. The first two show where shared behavior still needs a canonical implementation.

1. **[P2] Centralize tab replacement; Training still retains the reader’s old page on the stack.**  
   **[TrainingPanel.cxx:DrawTrainingPanel](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/TrainingPanel.cxx:623)**

   Open Training → Frame data → a player’s reader, then click another tab. `DrawHeader` allows tab clicks while reading, but Training handles the resulting shortcut with `nav.Return(); nav.Push(a.id)`. `Return()` closes the reader without popping its screen, so the new tab stacks over Frame data. Back consequently returns to the previous tab instead of the Training root.

   The shell fixes this with explicit cancellation before its own return/push sequence, leaving two implementations of the same transition with different guarantees.

   **Remedy:** give navigation one operation for replacing the current tab that cancels its modal and preserves the parent stack. Use it from both owners. Cover switching tabs from an open Training reader and returning once to the root.

2. **[P2] Hint clicks still implement a second, incomplete menu-action dispatcher.**  
   **[GameMenuDraw.cxx:GameMenu::DrawHintBar](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/GameMenuDraw.cxx:607)**

   The hint bar now displays the dialog’s labels, but its actions do not consistently follow the dialog’s input rules:

   - Select handles a choice with `Pick`, otherwise calls `Choose`. `Choose` immediately returns while confirming, so it cannot answer the highlighted confirmation.
   - Notice and editor hints are displayed while click handling explicitly rejects those states. Confirmation popups also block the underlying window’s pointer handling.
   - Screen shortcuts are appended even while a choice is open. Clicking a room’s Fighter hint can therefore leave an open popout, whereas pressing the physical Fighter button is ignored by navigation while choosing.

   This leaves menu-audit finding 6 only partly resolved. Correct labels do not establish a consistent action contract.

   **Remedy:** route hint activation through the same mode-aware action handling used by physical input. Put actionable dialog hints inside the dialog’s input surface, or render the background hints as noninteractive labels. Suppress screen shortcuts while the dialog owns them.

3. **[P2] The Room layout picture row still uses the locked-preferences policy.**  
   **[ApplicationShell__Settings.cxx:SettingsScreens::RoomLayout / PictureMode](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__Settings.cxx:217)**

   `RoomLayout` builds its picture row through `PictureMode(s, v)` and changes only its ID to `arrange-picture`. That builder sets `enabled = v.canEditPreferences`, which is false in a joined room. Column, split, preset, members, chat and reset work there; Picture remains disabled.

   The same preference is editable through the room’s More menu, and `CopyRoomSettings` now correctly permits saving it. The persistence fix is complete, but room-audit finding 16 remains partly open.

   **Remedy:** use one eligibility rule for local room-layout settings across Settings, board controls and saving. Extend the joined-room journey beyond Column to select Picture and observe its scoped save.

4. **[P2] Keep Character select reachable without a device-specific shortcut on deciding Arcade prompts.**  
   **[ApplicationShell__PostMatch.cxx:ApplicationShell::DrawPostMatch](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__PostMatch.cxx:188)**

   When `deciding` is true, the prompt contains Rematch, New opponent and Back. The Fighter row is deliberately omitted, leaving Character select available only through the Fighter shortcut.

   `MenuShortcuts` correctly gives DirectInput sticks no Fighter shortcut, and `ShortcutHints` consequently omits it. Those users cannot choose Character select from this prompt. The new device-capability model is sound; this caller does not respect it.

   **Remedy:** retain Character select as a navigable row in the deciding state. Treat the shortcut as an additional route. Adapt the button layout to the resulting count and cover the deciding prompt with DirectInput capabilities.

Three smaller, explicitly audited issues also remain:

- **Direction repeat remains split across consumers.** [MenuDialogs.cxx:ReadEditInput](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/MenuDialogs.cxx:357) uses rising edges only, as does notice navigation in `GameMenu::ReadFrame`. Choices now repeat, but notices and the on-screen keyboard still do not. Share one gated direction-repeat result across these consumers.
- **Secret draft copies remain unwiped.** [MenuDialogs.cxx:PressKey](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/MenuDialogs.cxx:332) copies the draft before the Done early return and does not wipe unchanged local copies. Apply a scoped wipe to every exit, including moved-from short strings. This is menu-audit finding 18.
- **Embedded screens still bypass the toast lifecycle.** [ApplicationShell.cxx:Draw](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell.cxx:689) returns early for fighter selection, post-match and reports before `DrawToasts`. Notifications received there remain delayed until another screen runs the common tail. Move toast collection and presentation into a frame-completion path used by every screen.

The round-one remedies otherwise check out:

| Remedy | Round-two disposition |
|---|---|
| Sol 1: physical LP/HP and reserved Select/Back | Implemented. Input and glyphs share `ShortcutButtons`; collision coverage includes screens with and without tabs. |
| Sol 2 / Opus 7: canonical Settings panels and screen ownership | Implemented. Direct panel builders replace recursive page construction and filtering; shell screens use `ScreenPanel`. |
| Sol 3 / Opus 4a: retire fighter page graph | Implemented. Retired pages, state, previews and catalog helpers are removed; assertions target the board. |
| Sol 4 / Opus 1: in-room layout persistence | Copy contract, restored self-test, realistic joined-room harness and save/acknowledgement journey are present. Picture-row eligibility remains finding 3 above. |
| Sol 5: shared row geometry | Implemented. Drawing and pointer adjustment share `MenuRowLayout`, including value Y bounds; stacked-row coverage is restored. |
| Opus 2: replace board neighbour wiring | Implemented through `MenuEdge`; the four cited boards no longer retain those link tables. |
| Opus 3: shortcut table and hint factory | Implemented, including keyboard bindings and removal of the empty tab branch. |
| Opus 4b / 9c: one renderer and no cursor clamp | Implemented through `GameMenu::Draw` and its frame structure. `HintBar` creates no ImGui items. |
| Opus 5: JSON serialization and UTF-8 decoding | Implemented. The examined Iroh helper and checkpoint messages use `DumpJson`; Identity and release-note text share the decoder. |
| Opus 6: typed row IDs | Implemented. Keyed UI parsing uses optional `RowKey`; the remaining throwing conversions found were in test utilities, not UI handlers. |
| Opus 8: shared board rows | Implemented for Create, Settings and fighter selection. |
| Opus 9: pointer boundary and acting host | Implemented. Pointer movement is sampled during drawing; room rows distinguish acting-host identity from current mutation eligibility. |

The file-size ruling is satisfied: `ApplicationShell.cxx` is **839 lines**, and `RoomPanel.cxx` is **725**, with tables and column rendering extracted. No changed file moves from below 1,000 lines to above it. The Classic room type and Classic frame-meter style remain. I found no additional blocker in the shared Arcade/King rotation or the three Bugsink crash fixes.

Validation used existing binaries only:

- **16 non-render test executables passed**, covering navigation, menu buttons, room navigation, Arcade, authority, recovery, gamepad slots, country selection, UI polish, fighter catalog, native selection, identity events, localization, pad setup/mapping and speech.
- Headless `UiRender` passed **4,365 frames at 1920×1080/100%** and **4,235 frames at 1280×720/100%**.
- CTest could not create its log under the read-only permissions. Direct `ShellJourney` execution failed while creating its temporary directory. `SettingsStore` terminated with **0xC0000409**, without diagnostics; its result remains unverified.
- The examined binaries are newer than the changed sources, but the provenance manifest still identifies an older nightly revision. These results do not establish a fresh build of this commit or gameplay acceptance.

The checkout remains clean. No files were edited and no build was run.

VERDICT: CHANGES REQUIRED

