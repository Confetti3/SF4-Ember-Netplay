Reviewed all eight commits in `fcd33f73..6dd2ec113`. Five findings block approval under the requested quality bar.

1. **[P1] Resolve DirectInput shortcuts from buttons that remain available after Select and Back bindings.**  
   **[ControllerNavigation.hxx:ControllerButtons / MenuShortcutGlyph](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ControllerNavigation.hxx:48)**

   The Xbox path excludes `menu.confirm | menu.back`; the new DirectInput path reads LP/HP from `held` without applying that exclusion. A supported configuration—binding Menu confirm to the physical button that produces LP—therefore emits both `Select` and `PrevTab`. `MenuNavigation::Update` handles shortcuts before Select, so the confirmation becomes a tab change, or does nothing on a screen without tabs. The glyph resolver also continues advertising LP as a tab button.

   **Remedy:** resolve available shortcut bindings once, using the device’s physical mapping and its reserved Select/Back buttons. Feed both input decoding and glyph generation from that resolved result. Cover rebound Select and Back on LP/HP, including on a screen without tabs. The existing collision test covers Xbox LB only.

2. **[P2] Delete the legacy-page adapter underneath Settings instead of making it the permanent Settings architecture.**  
   **[ApplicationShell__Settings.cxx:SettingsTabRows](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__Settings.cxx:26)** and **[ApplicationShell.cxx:BuildRows](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell.cxx:300)**

   Settings still calls the shell’s large screen dispatcher to construct complete old pages, then filters their rows by string IDs and prefixes. The Interface tab constructs the entire Interface page four times per frame. The Player tab constructs the old Settings menu just to retain its Country row. Rendering then reconstructs panel boundaries from adjacent translated `group` strings.

   This preserves a substantial compatibility layer after the alternative UI has been removed. Adding a setting still requires understanding synthetic pages, filtering rules, panel grouping and the shell dispatcher.

   **Remedy:** make Settings panels the canonical model, with stable panel IDs and focused row builders. Build each panel once and use that result for the rail, summaries, open-panel rows and previews. Reuse those builders for any genuinely reachable standalone pages. Remove the obsolete menu scaffolding and recursive `BuildRows` adapter. This is a concrete opportunity to delete complexity while reducing `ApplicationShell.cxx`.

3. **[P2] Remove the unreachable Classic fighter-selection journeys and their state.**  
   **[FighterSelector.cxx:FighterSelector::Draw](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/FighterSelector.cxx:311)** and **[FighterSelector.hxx:FighterSelector](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/FighterSelector.hxx:23)**

   The live entry paths now open the board and focus its Fighter, Costume or Ultra row. Nevertheless, the implementation retains the old `roster`, `appearance`, `costumes`, `colors` and `ultra` pages, their separate previews and action branches, and the deferred roster-to-Ultra workflow. Those flows are sustained by tests that directly push their screen IDs. `CurrentPage()` has no callers, while `page_`, `focusSavedUltra_` and `ultraStepPending_` remain.

   **Remedy:** retain the board and its reachable Stage, Random pool and Options pages; delete the retired page graph, associated state, rendering branches and unused strings. Move the relevant ownership, DLC, custom-content and saved-selection assertions onto journeys through the live board. Keeping tests of inaccessible screens perpetuates the implementation the removal should eliminate.

4. **[P2] The newly enabled room-layout controls do not satisfy the in-room persistence contract.**  
   **[ApplicationShell.cxx:BuildRows / UpdatePreferenceSave](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell.cxx:355)** and **[PlayerPreferences.hxx:CopyRoomSettings](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/netplay/PlayerPreferences.hxx:203)**

   The branch enables Column, Split, Preset, Members, Chat and Reset inside a room. Their handlers change `roomColumn`, `roomSplit` and the visibility flags. However, `CopyRoomSettings` now copies only button layouts. Consequently, `OnlyRoomSettingsChanged` rejects these edits, and `UpdatePreferenceSave` cannot submit them while normal preference editing is unavailable. The controls visibly change the layout but leave the edits pending until the room restriction ends.

   **Remedy:** put the supported local room-layout fields into the canonical in-room preference-copy contract. Use that same contract for save eligibility and runtime application. Add a journey that changes a layout setting while joined, observes the scoped save request, and acknowledges the resulting snapshot; checking that the row is enabled is insufficient.

5. **[P2] Share complete row geometry between drawing and hit-testing.**  
   **[GameMenuV2.cxx:DrawMenuRow / MenuValueStep / PressEntry](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/GameMenuV2.cxx:142)**

   The new layout can place a value beneath its label, but `PressEntry` receives only `valueWidth`, and `MenuValueStep` examines only the pointer’s X coordinate. The list renderer duplicates that same calculation. A click on the label above an arrow or switch can therefore adjust the value beneath it. Drawing knows the value’s lower-line rectangle; hit-testing does not.

   **Remedy:** calculate a row layout containing the label and value rectangles once, and pass it to both drawing and interaction. Restrict adjustment clicks to the actual value rectangle, including its Y bounds. Restore the stacked-row label-versus-arrow interaction coverage removed from `DialogContract`, adapted to V2.

**File-size ruling:** neither file crosses the 1,000-line threshold in this branch. `ApplicationShell.cxx` shrinks from **1,195 to 1,175 lines**, and `RoomPanel.cxx` from **1,355 to 1,120**. Both should still be decomposed here. The Settings restructuring above gives the shell a meaningful split. For `RoomPanel.cxx`, extract board rendering and layout into `RoomBoard.cxx`, then build the board’s rows directly in their final sections instead of constructing a mixed list and reclassifying it into `kept`, `training`, `copies` and `asks`. That reduces both file size and the number of representations a maintainer must follow.

The shared Arcade/King rotation path, recovery digest/size/wire use of `DumpJson`, shared UTF-8 decoder, `RowIndex` parser and toast gate ownership are sound directions. The deleted English localization keys have no remaining literal references in the scanned C++ sources.

Validation: seven existing executables passed—`RoomArcade`, `ControllerNavigation`, `RoomAuthority`, `SessionRecoveryGate`, `GamepadSlots`, `RoomPanelNavigation` and `CountryPicker`. The quick headless UI run passed **4,329 frames** at 1080p/100%. CTest logging and ShellJourney’s temporary directory were blocked by read-only permissions. The full headless matrix exited zero but supplied no captured completion summary, so I am not counting it as a verified full-matrix pass. These are existing-binary results; the provenance manifest records an earlier nightly revision. No build, edits, deployment or gameplay validation was performed, and the checkout remains clean.

VERDICT: CHANGES REQUIRED