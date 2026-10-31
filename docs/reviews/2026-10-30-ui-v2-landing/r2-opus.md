# UI V2 landing, review round 2 (Opus)

Scope: `git diff fcd33f73..HEAD` on feat/ui-v2-lands at 039e5cab, read file by file, with the two commits since round 1 (ea46fa639, 039e5cab4) read in full and the resulting files read where the shape matters: ApplicationShell.cxx, ApplicationShell__Settings.cxx, ApplicationShell__ShellScreens.cxx, ScreenPanel.hxx, GameMenu.cxx, GameMenu.hxx, GameMenuDraw.cxx, MenuNavigation.hxx, ControllerNavigation.hxx, common/MenuButtons.hxx, RowId.hxx, RoomPanel.cxx, RoomPanelTables.cxx, RoomPanelColumn.cxx, RoomPanelBoard.hxx, FighterSelector.cxx, CountryPicker.cxx, PlayerPreferences.hxx and the session JSON and UTF-8 helpers. Standard: r2-prompt.md.

Evidence run, read only: `ctest --test-dir build\current -R "^(ControllerNavigation|MenuButtons|ShellJourney|IdentityJourney|RoomPanelNavigation|NativeSelectionAvailability|CountryPicker|V2Layout|ControllersScreen|RoomArcade|RoomAuthority|IdentityEvents|SessionRecoveryGate|FighterCatalog|WatchJourney|RankingsJourney|PublicRoomsJourney)$"` passed 18 of 18. No .cxx or .hxx under src is newer than build\current\ShellJourneyTest.exe, so the binaries cover the reviewed source. No source file crosses 1000 lines in this branch (ApplicationShell.cxx 1195 to 839, ApplicationShell.hxx 830 to 852, MenuNavigation.hxx 388 to 516, RoomPanel.cxx 1355 to 725).

## Round 1 findings

Opus 1 / Sol 4 (in-room layout saves). Resolved at the contract: CopyRoomSettings carries roomColumn, roomSplit, roomShowMembers, roomShowChat, pictureMode and pictureSpot; sf4e__SelfTest__RoomLayout.cxx and its IN_GAME_SELFTEST.md section are back and test those fields; tests/shell_journey_support.hxx:JoinedRoom clears canEditPreferences; shell_room_journey_test.cxx:RoomLayoutSavesInRoom checks that Column goes out as a roomSettings request, not SavePreferences, and that the acknowledged snapshot lets the next change go too. One residual in the Settings rows, finding 1 below.

Opus 2 (neighbour wiring). Resolved. MenuEdge { Stay, Flow } is a per-screen state in MenuNavigation, Update falls back to NextPlaced under Flow, and the wiring on Create (RoomSetupRows), CountryPicker::BoardRows, FighterSelector and WelcomeRows is gone. The only explicit neighbours left are on unplaced rows (Home's card, RankingsPanel, the CJK country list), as round 1 asked. BoardGeometry covers Stay, Flow, both ends and that Flow never overrides a row drawn that way.

Opus 3 (one shortcut table). Resolved. MenuShortcuts carries the ImGuiKey and KeyboardMenuBits reads it; the room's Leave hint is ShortcutHint(MenuInput::Leave, ...); PadSetupLegend marks its own Escape as Back; GameMenu::Finish drops an untaken TabDelta, and the shell's empty branch is gone.

Opus 4 (Classic leftovers). Resolved. FighterSelector's retired pages, page_, CurrentPage, ultraStepPending_, focusSavedUltra_ and RosterGridColumns are gone; DrawV2 is folded into GameMenu::Draw over a Frame struct with ReadFrame, DrawHeader, DrawBody, DrawCards and DrawHintBar parts, and backRequested lives on the frame; the orphan 820-line comment and every "V2:" comment, DrawV2Row and the "V2 list/tables/members/chat" child names are gone.

Opus 5 (DumpJson and Utf8Text). Resolved. Every IrohRoom helper message and the checkpoint goes through DumpJson, and JsonDump.hxx now says so truthfully. The two raw dumps left in src/session (IdentityEvents.cxx body, TournamentAnswers.cxx ticket) are outside room and recovery code and each sits in its own try. IdentityEvents.cxx:Text uses Utf8Head and ReleaseNotesText.hxx:DrawableUtf8 uses Utf8CharacterBytes with its code point.

Opus 6 (RowIndex). Resolved. RowKey<Key> returns std::optional, rejects signs and values the key type cannot hold, and every caller uses the if-initialiser form, the 64-bit member ids and `rkb-p:` and `slot-` included. No std::stoi, stoul or stoull is left in src/ui.

Opus 7 / Sol 2 (Settings model, shell decomposition). Resolved for Settings and the shell's own screens, partial for the rest; see finding 3. SettingsPanel with one builder per panel, built once a frame into settingsPanels_, replaces the recursive BuildRows adapter and the id and prefix filters; Online, Create, Join, the welcome and a room's screens sit behind ScreenPanel in ApplicationShell__ShellScreens.cxx. Sol's "stable panel IDs" was not taken up; see finding 2.

Opus 8 (board value rows). Resolved. GameMenu::DrawBoardRow and BoardRowHeight serve Create, Settings and fighter select, and nothing outside GameMenuDraw.cxx calls FitMenuRow any more.

Opus 9. Resolved: CountryPicker reads MouseDelta in DrawBoard into pointerMoving_; RoomScreenRows carries actingHost and RoomTableRows no longer recomputes it; the CursorMaxPos clamp is gone (the one imgui_internal call left in GameMenuDraw.cxx is BringWindowToDisplayFront for the value panel).

Sol 1 (stick shortcuts on Select or Back). Resolved. input::MenuButtons resolves lightPunch and heavyPunch from the fight table, ControllerNavigation.hxx:ShortcutButtons removes the device's confirm and back from every shortcut, and both ControllerButtons and MenuShortcutGlyph read it. Tests cover Select on LP and Back on HP. One edge case remains in the same function, finding 4.

Sol 3 (retired fighter pages). Resolved, as Opus 4 above; native_selection_availability_test and the selection journeys now go through the board.

Sol 5 (row geometry). Resolved. MenuRowLayout, LayOutMenuRow and MenuValueStep give drawing and the pointer one layout with the value's Y bounds, and DialogContract has stacked-row label versus arrow coverage for both a list card and a board row.

Sol file-size ruling. Resolved. RoomPanel.cxx holds rows and actions, RoomPanelTables.cxx the board and table cards, RoomPanelColumn.cxx the Members over Chat column, and RoomBoardRows builds rows in draw order with no reclassification pass (no `kept`, `training`, `copies` or `asks`). DrawRoomBoard is about 110 lines.

## Findings, in priority order

### 1. The settings a room leaves open are stated twice, and the two statements disagree

netplay/PlayerPreferences.hxx:CopyRoomSettings now says the watched game's picture (pictureMode, pictureSpot) is open in a room, beside the board's layout. The Settings rows decide the same thing on their own, by hand, and say otherwise:

- ApplicationShell__Settings.cxx:SettingsScreens::RoomLayout hard codes `enabled=true` on the five `arrange-*` rows and Reset, but its sixth row, `arrange-picture`, comes from PictureMode(s, v), whose enabled is `v.canEditPreferences`. In a room the Room layout panel therefore shows Picture disabled with the leave-the-room reason, between rows that work, while the board's own More… picture choice (RoomActions.cxx:RoomAction, `picture-N`) changes the same field and saves it through the room path.
- SettingsScreens::TrainingReplays gates `watched-picture`, `picture-corner` and `picture-size` on canEditPreferences, and SettingsScreens::PictureSpot ignores a SpotMap pick unless canEditPreferences, although pictureSpot is in the room set and ApplicationShell::Draw's corner window already saves a dragged pictureSpot in a room.

The literal `true` hides the invariant rather than stating it: nothing ties those rows to CopyRoomSettings, so the next field added to the room set will be wrong in one place or the other, as pictureMode is now.

Remedy: state the rule once. Either add a `canEditRoomSettings` gate to ShellView, computed where canEditPreferences is (true when canEditPreferences, or at the main menu in a room and not fighting, the same condition UpdatePreferenceSave and the runtime's CanEditButtonLayoutNow use), or give the Settings builders one helper that answers it. Use it for every row whose field is in CopyRoomSettings: the six Room layout rows, `watched-picture`, `picture-corner`, `picture-size` and the PictureSpot SpotMap. Extend RoomLayoutSavesInRoom with an `arrange-picture` press so the journey covers a picture field, not only Column.

### 2. Settings panels are addressed by position, not by a stable id

A panel's screen is its index on its tab: "tab-interface/2" is "the third panel of Interface". Sol's round 1 remedy asked for stable panel ids, and the code went the other way. The index now appears in 41 references across src: MenuWalk.cxx (`{"settings panel", "tab-access/1", ...}`, product code), nine journey and render test files, and SettingsScreens::Dress, whose eyebrow indexes settingsPanels_ by it. Inserting, removing or reordering one panel in SettingsScreens::Tabs silently retargets the menu walk and every journey after it on that tab, and each journey still passes or fails for the wrong reason. The index is also parsed by hand twice with std::atoi (SettingsScreens::OpenPanel and SettingsScreens::DrawBoard's `current`) in a file that includes RowId.hxx; DrawBoard's parse turns any focused id that is not a head, `retry-save` for one, into panel 0 by accident.

Remedy: give SettingsPanel a short id beside its heading ("room-layout", "match-hud", "reports"), make a panel's screen "tab-interface/room-layout", and have OpenPanel and DrawBoard find the panel by that id among the tab's panels. MenuWalk and the journeys then name what they open, and both atoi parses go. If the index is kept for now, at least parse it through RowKey<std::size_t> and bound it once in one helper.

### 3. Draw and UpdateStatus still dress screens whose panels already exist (Opus 7 residual, not blocking)

ScreenOf's owner table includes PublicRoomsScreens, RankingsScreens and ReplaysScreens, yet ApplicationShell.cxx:Draw still sets their parts inline: public rooms' eyebrow, region chip, paneWidth 380, the toolbar grid and the opening-row focus; rankings' tabs and Refresh hint; replays' card and detail lambdas, customDetail, paneWidth, tabs, eyebrow and header chip. ApplicationShell.cxx:UpdateStatus likewise keeps the language save error for `tab-access` and about 60 lines of room, room-chat and room-table status beside the `owner->Status` call made for exactly that, and BuildRows keeps the `tournament-matches` focus fix-up after IdentityScreens' own Rows. Remedy: move each into the owning panel's Dress, Card, Detail, Grid, Rows or Status override (the languageSaveError line into SettingsScreens::Status, the room lines into RoomScreens::Status, in the same order so precedence is unchanged). Draw is then the frame loop the ScreenPanel comment already says it is. Round 1 called this owed rather than blocking, and it stays so.

### 4. A stick's tab glyph can name a 3P button that never steps a tab (low priority)

ControllerNavigation.hxx:ShortcutButtons returns, for PrevTab, every physical button whose fight function includes LP, minus Select and Back. A 3P button holds both LP and HP, so it appears in both tabs' sets, and ControllerButtons then cancels the pair. When the player has put Select on the LP button and Back on the HP button, the only button left in either set is 3P: MenuShortcutGlyph still returns "LP" and "HP", so the hint bar and tab strip advertise tab buttons that do nothing. menu_buttons_test.cxx:Resolver asserts that glyph in exactly that configuration. Remedy: in ShortcutButtons, drop the buttons that hold both tab functions (`menu.Holding(s.stick) & ~(menu.lightPunch & menu.heavyPunch)`), keep the both-tabs cancel for two separate buttons pressed together, and change the test to expect no tab glyph there.

## What unblocks approval

Findings 1 and 2 are each small and concrete. Finding 1 is a correctness gap in the area both round 1 reviews blocked on, and finding 2 is a stated round 1 remedy that was not taken. Fixing those two is enough for my approval; 3 and 4 can follow.

VERDICT: CHANGES REQUIRED
