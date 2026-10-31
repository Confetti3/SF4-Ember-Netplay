# UI V2 landing, review round 1 (Opus)

Scope: every commit in `git diff fcd33f73..HEAD` on feat/ui-v2-lands (8 commits, 138 files, +3902 / -5041), read file by file: src/ui, then src/session, src/common, src/netplay, src/sf4e, tests, docs and locales. Standard: docs/reviews/2026-10-30-ui-v2-landing/r1-prompt.md.

Evidence run, read only: `ctest --test-dir build\current -R "^(ControllerNavigation|RoomPanelNavigation|ShellJourney|RoomArcade|RoomAuthority|SessionRecoveryGate|GamepadSlots|NativeSelectionAvailability|SettingsStore)$"` passed 9 of 9. No .cxx or .hxx under src is newer than build\current\ShellJourneyTest.exe, so the binaries cover the reviewed source.

Things checked and found clean, so they are not findings:

- Strings. All 40 locale keys the branch removed are gone from all 14 catalogues (each now has 2666 msgids), no source line still references a removed key, and every `loc::T`/`loc::Tf` literal in src exists in en.po.
- `v2::On`, `v2::SetOn`, `lookV2`, `BoardLayout`, `stableStatus`, `fitStatus`, `wideListShare`, `cardRounding`, `compactDetail*`, `selectSteps`, `MenuLegend`, `DialogButtonsHeight`, the unread badge, `roomSideWidth`, `roomMemberShare`, `roomMembersHidden` and `roomChatHidden` have no remaining references.
- ArcadeSetEnded no longer calls JoinTable for a King pair with nobody else seated, but its only caller, RoomModel.cxx:RoomAuthority::CompleteSet, zeroes the score before calling it, so the one rotation path does not carry a score over.
- The two big files were not pushed past 1000 lines by this branch: ApplicationShell.cxx went from 1195 to 1175 and RoomPanel.cxx from 1355 to 1120.

## Findings, in priority order

### 1. The room board's layout is edited in a room but is not one of the settings a room leaves open

netplay/PlayerPreferences.hxx:CopyRoomSettings now copies `buttonLayouts` alone. Before the branch it also copied the Classic board's column (`roomSideWidth`, `roomMemberShare`, `roomMembersHidden`, `roomChatHidden`). The branch deleted those fields with the Classic look, but did not move the board that replaces them into the set: `roomColumn`, `roomSplit`, `roomShowMembers`, `roomShowChat` and `pictureMode`, which RoomGrips, the panels' Hide and Show (RoomActions.cxx:RoomAction, `room-hide-*`, `room-show-*`, `picture-N`) and the Interface tab's `arrange-*` rows change, stay outside it. At the same time ApplicationShell.cxx:BuildRows now builds every `arrange-*` row with `enabled` hard coded to `true`, so they are live inside a room, and the commit message says "room layout rows work in a room".

In the runtime `canEditPreferences` is false whenever a room is open (sf4e__NetplayRuntime__Status.cxx:486). ApplicationShell.cxx:UpdatePreferenceSave then saves only when OnlyRoomSettingsChanged holds, and it does not hold for any of these fields. A layout change made in a room therefore stays unsaved until the room is left, and while it is pending it also blocks the button-layout autosave that the room path exists for. The Button layout screen then says Saving until the player leaves.

Nothing catches this. The branch deleted sf4e__SelfTest__RoomLayout.cxx and its section in IN_GAME_SELFTEST.md, which was the only check that layout settings save from inside a room. The shell harness (tests/shell_journey_support.hxx:37, unchanged by this branch) sets `canEditPreferences=true` for every view and leaves it on after JoinedRoom, which the runtime never does, so shell_selection_journey_test.cxx:RoomPartsHide saves through the free path.

Remedy: make CopyRoomSettings copy the board's own layout fields (`roomColumn`, `roomSplit`, `roomShowMembers`, `roomShowChat`, `pictureMode`, `pictureSpot`). Retarget the RoomLayout self-test to those fields instead of deleting it, and restore its doc section. Make the harness's joined room clear `canEditPreferences` so RoomPartsHide and an `arrange-column` case go through the `roomSettings` request.

### 2. The new geometry navigation is contradicted by per-screen neighbour wiring, one of which this branch adds

MenuNavigation.hxx:Update and PressEntry's `nav.Place` are meant to be the one board navigation rule, and docs/design/V2_KIT.md now says "Do not wire neighbours by id on a board; draw the rows where they belong." Update still checks a row's explicit `left/right/up/down` before `Placed()`, and several boards that place every row also wire neighbours by id. On those boards the geometry never runs:

- ApplicationShell__Create.cxx:RoomSetupRows is new in this branch and chains every row's `up` and `down` by id, with a comment saying Up and Down should ignore where the rows are drawn.
- CountryPicker.cxx:BoardRows sets all four neighbours on every letter and country, pointing an edge back at the row itself, which is what Nearest already does.
- FighterSelector.cxx:Draw wires the grid's right edge to `costume` and Ready's `left` to the saved fighter.
- ApplicationShell__FirstRun.cxx:WelcomeRows wires Skip and Next.

The room board's hand-wired cabinet table was deleted in favour of geometry, so the branch already shows the path. It just was not finished.

Remedy, the code-judo move: give MenuNavigation one per-screen choice for what happens when Nearest finds no placed row in the pressed direction: stay (today's default, which is all CountryPicker's edge-to-self wiring asks for) or flow to the next or previous placed row in entry order (what the Create comment wants, from the foot of the Room panel to the top of the Rules panel). One flag replaces four link tables. Then delete the wiring on the four boards and the Create comment. Right from the end of a letter row already reaches the country list beside it by geometry, so CountryPicker needs nothing more. Explicit neighbours then stay only where nothing is placed: GameMenu lists, Home's player card in the pane (an InvisibleButton, not PressEntry) and RankingsPanel. The doc then describes the code.

### 3. "One shortcut table" is bypassed in four places

ControllerNavigation.hxx:MenuShortcuts owns each shortcut's pad button, stick function and glyph, but not the keyboard binding. GameMenu.cxx:KeyboardMenuBits keeps its own `menuOnly` table binding ImGuiKey_F, T, C, Delete, PageUp and PageDown to the same presses. The table's `key` column ("F", "T", "Del", "PgUp") is therefore display text that can drift from the real binding. Beside it:

- ApplicationShell.cxx:Draw inserts the literal `{"Del",placeExit,MenuInput::Leave}` instead of asking MenuShortcutGlyph(MenuInput::Leave).
- ApplicationShell__PadSetup.cxx:DrawPadSetup sets `menu_.shortcutHints.front().press` by position, assuming PadSetupLegend puts Escape first.
- ApplicationShell.cxx:Draw has an empty `else if(TabDelta(a)){}` branch whose only job is to keep an untaken LB or RB away from RoomShortcut and the owner's Shortcut.

Remedy: add the ImGuiKey to MenuShortcuts and build KeyboardMenuBits' shortcut rows from it. Use MenuShortcutGlyph for Leave. Have PadSetupLegend mark its Escape hint's press itself. Drop an untaken TabDelta in GameMenu::Finish after the body has had it, which deletes the shell's empty branch and spares every other owner (TrainingPanel's tabs, the room board's narrow tabs) the same check.

### 4. Classic removal left dead code and a seam it no longer needs

- FighterSelector.cxx:Draw. The `roster`, `appearance`, `costumes`, `colors` and `ultra` pages cannot be reached in the product. The board on `home` replaced them, `openOn_` values `roster`, `ultra` and `costumes` now only Prefer a row on the board, and no row on the board pushes those screen ids (costume, ultra and edition open their choices; color only steps). Only native_selection_availability_test.cxx reaches them, by pushing the screens directly. With them go the `!board` Ultra step, `ultraStepPending_`, `focusSavedUltra_`, the gallery branch of the card lambda, PreviewColor's gallery use, `page_` and `CurrentPage()` (which has no reader at all), and `selection::RosterGridColumns`, now read only by fighter_catalog_test. Remedy: delete the pages and their state, and move the test's costume to colors coverage onto the board's costume and color rows.
- GameMenu.cxx:Draw computes input, gates and narration, then hands 17 parameters to GameMenuV2.cxx:DrawV2, the only renderer. The split existed so two looks could share the front half. Draw also still declares `bool backRequested=false` under "Settled after the body", and nothing reads it. Remedy: fold DrawV2 into Draw, or pass one frame struct and name it for what it is (DrawScreen, GameMenuScreen.cxx). Delete the dead local. Since every Draw call site was touched for `flyoutScale` to `flyout` and `stableStatus` anyway, a small options struct would also replace the positional `false, 100, Tone` tails.
- ApplicationShell.hxx keeps the two-line comment about the board "last drawn wide on a screen under 820 lines" with no member under it, and it now runs into the createFocus_ comment.
- About 20 comments still begin "V2:" or "V2's" as if a mode existed: MenuNavigation.hxx (lines 21, 56, 89 and 108), MenuRows.hxx, ReplaysPanel.hxx and .cxx, RoomActions.cxx (62 and 69), RoomPanel.cxx:63, TrainingPanel.cxx:184, RecoveryMenu.hxx, CountryPicker.hxx, FighterSelector.hxx, ApplicationShell__Create.cxx:159 and ApplicationShell__Values.cxx:1. RankingsCards.cxx still names DrawV2Row, and child windows are still called "V2 list", "V2 tables", "V2 members" and "V2 chat". Remedy: one mechanical pass. The `v2::` kit namespace and the `room.v2.*` locale keys can stay; the comments and function names should stop implying another look.

### 5. The session layer's new helpers claim more than the branch made true

- common/JsonDump.hxx calls DumpJson "the one way room and recovery code serializes JSON that can carry text from a player or a peer". SessionServer, SessionClient, SessionRecovery and IrohRoom's effect payloads now use it, but IrohRoom.cxx still calls `.dump()` on externally sourced strings. Join's `request.dump()` (line 123) carries the invitation, which SessionController.cxx:Execute only checks for length. JoinPublic (line 132) carries the public ticket. IrohRoomPublic.cxx carries `ban_account`'s account. IrohRoomPeers.cxx:338 carries the send header's peer identity. These are the EMBER-63 failure class. Remedy: route those sites through DumpJson as well, which is mechanical, or narrow the header to say which sites are exempt and why. As written, a reader cannot tell a safe raw dump from a missed one.
- common/Utf8Text.hxx introduces "one decoder", but IdentityEvents.cxx:Text, three lines above the new Utf8Trim calls in the same file, still walks continuation bytes by hand and passes ill-formed bytes through. common/ReleaseNotesText.hxx:DrawableUtf8 is a third decoder with its own overlong and surrogate checks. The file now cuts strings two ways: Text by characters, unvalidated, and Utf8Trim by bytes, validated. Remedy: implement Text with Utf8CharacterBytes (or add a character-count sibling of Utf8Trim to Utf8Text.hxx), and let DrawableUtf8 use Utf8CharacterBytes for validity, keeping only its Displayed filter.

### 6. RowIndex stops short of the commit's claim, and its sentinel invites double parsing

ui/RowId.hxx:RowIndex returns int, so the 64-bit member ids were left on std::stoull: RoomActions.cxx:RoomAction (lines 64 and 152) and RoomPanel.cxx:RoomEntries (223) and RoomMemberCard (630). RankingsCards.cxx:DrawRankingsRow (134, `rkb-p:`) and TrainingPanel.cxx:DrawTrainingPanel (650, `slot-`) also still use stoul and stoi. No text row shares those prefixes today, so none can throw yet, but this is the same latent EMBER-68 pattern the commit says is gone. Separately, the -1 sentinel produces `RowIndex(id,"x-")>=0?RowIndex(id,"x-"):fallback` and `if(RowIndex(a.id,p)>=0){... RowIndex(a.id,p) ...}` about a dozen times (ApplicationShell.cxx, ApplicationShell__PadSetup.cxx, FighterSelector.cxx, RoomActions.cxx), while FighterSelector.cxx also uses the `if(const int n=RowIndex(...);n>=0)` form. Remedy: return `std::optional<int>`, or template it on the integer type, and use the if-initialiser form throughout. For member rows, read `MenuEntry::key`, which RoomBoardRows and RoomMembersRows already set to the member id for exactly this purpose.

### 7. ApplicationShell.cxx and RoomPanel.cxx: not blocking here, owed next

Both shrank, so the over-1000 rule does not bite this branch. But the removal turned `v2::On()&&screen==...` chains into plain `screen==...` chains rather than removing them. ApplicationShell.cxx:Draw is about 440 lines, and roughly 200 of them choose per screen the eyebrow, header chips, tabs, listWidth, paneWidth, Card, Detail and Body (online, create, welcome, awards, the settings tabs, replays, news, country picker). ScreenPanel.hxx already models a screen's owner with Owns, Rows, Card, Detail and Shortcut. Adding Header and Body hooks and moving those screens behind it deletes the chains rather than moving them, and leaves Draw as the frame loop. RoomPanel.cxx:DrawRoomBoard is about 360 lines in one function with four separable parts: the Classic table cards, the Members panel, the Chat panel and the action bar. The branch's own RoomPanelArcade.cxx is the precedent for RoomPanelTables.cxx and RoomPanelColumn.cxx, leaving the row builders in RoomPanel.cxx. I would not hold this branch for either split, but they should be the next change, before more screens land in Draw.

### 8. Value rows on boards are laid out three times by hand

FitMenuRow and v2::Share are a good single rule, but each board repeats the same sequence: FitMenuRow, a box grown by `UnderLine*s` when the value goes under, PressEntry with `share.value`, the ground, DrawMenuRow with the share, Focus, and `bodyChoices` when the row has choices. That sequence appears in ApplicationShell__Create.cxx:DrawCreateBoard (with a pre-pass to size the panel), ApplicationShell__Settings.cxx:DrawSettingsBoard and FighterSelector.cxx:DrawBoard. Remedy: one `BoardValueRow(nav, rows, e, at, width, rowHeight, changed, enabled, column)` in GameMenuV2.cxx that returns the row's box, plus a `BoardRowHeight(e, width)` for the pre-pass.

### 9. Smaller boundary points

- CountryPicker.cxx:BoardRows reads `ImGui::GetIO().MouseDelta` inside a row builder to decide which letter's list to build. Pointer state leaks into what should be a function of focus and preferences. Pass `pointerMoved` in from DrawBoard, or keep the shown letter in DrawBoard.
- RoomPanel.cxx:RoomTableRows recomputes `s.localMember!=room::ActingHost(s)` beside `c.host` (which also folds in `mutableRoom`), so the function holds two notions of host. Add an `actingHost` field to RoomScreenRows next to `host`.
- GameMenuV2.cxx:DrawV2 clamps `window->DC.CursorMaxPos.y` through imgui_internal to stop the hint bar from scrolling the window. If it stays, it belongs in one named helper with the reason. The cleaner fix is to give the hint bar its own child or draw it on the foreground list.

VERDICT: CHANGES REQUIRED
