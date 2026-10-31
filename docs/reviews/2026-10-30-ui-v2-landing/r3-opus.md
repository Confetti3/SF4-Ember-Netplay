# UI V2 landing, review round 3 (Opus)

Scope: `git diff fcd33f73..HEAD` on feat/ui-v2-lands at 2e5493db, with the one commit since round 2 (2e5493dbe, `git diff 039e5cab..HEAD`) read in full and the resulting files read from HEAD where the shape matters: ApplicationShell.cxx (Draw, DrawScreen, UpdatePreferenceSave, UpdateStatus, Background), ApplicationShell.hxx, ApplicationShell__Settings.cxx, ApplicationShell__Screens.cxx, ApplicationShell__PostMatch.cxx, ApplicationShell__ButtonLayout.cxx, ApplicationShell__Toasts.cxx, GameMenu.cxx (ReadFrame), GameMenuDraw.cxx (DrawHeader, DrawHintBar), MenuDialogs.cxx (PressKey, ReadEditInput), MenuNavigation.hxx, ControllerNavigation.hxx, TrainingPanel.cxx, the runtime's CanEditButtonLayoutNow, and the changed tests. The uncommitted crash-dump work under src/platform, src/netplay, src/launcher, scripts, server and locales was not reviewed, except where committed content depends on it (finding 1). Standard: r3-prompt.md.

Evidence, read only: `ctest --test-dir build\current -R "^(ControllerNavigation|MenuButtons|ShellJourney|IdentityJourney|RoomPanelNavigation|SettingsStore|V2Layout|RoomArcade|WatchJourney|RankingsJourney|PublicRoomsJourney|ControllersScreen|CountryPicker)$"` passed 13 of 13. The binaries were built at 22:42 to 23:01, after the last src/ui edit, and src/ui at HEAD matches the working tree except RecoverySurface.hxx (the other task), so these runs cover the committed UI code. They do not cover HEAD's SettingsStore: that binary was built against the uncommitted BoolPreferences.hxx, see finding 1. No build was run and no file other than this one was written.

## Round 2 findings

Opus 1 / Sol 3 (one rule for the settings a room leaves open). Resolved. ShellView::CanEditRoomSettings is the one predicate; RoomLayout's seven rows, PictureMode (now taking the row id, so the `rows[5].id` patch is gone), picture-corner, picture-size, PictureSpot's SpotMap, CanEditLayout and the save gate MaySave all read it, and the literal `true`s are gone. Reset now restores from a default-constructed PlayerPreferences, so the board layout and the picture come back together without repeating the default constants. RoomLayoutSavesInRoom checks every Room layout row is open in a room, that Picture goes out in the room's own request and not SavePreferences, and that the Matches picture rows are open while `replay-share` is not.

Opus 2 (panel ids). Resolved. SettingsPanel carries an id, a panel's screen is `tab-matches/training-replays`, SettingsScreens::Built finds the built panel by id, and both std::atoi parses and `<cstdlib>` are gone. DrawBoard now draws no panel while Retry saving has the focus instead of falling to panel 0. MenuWalk and every journey name the panel they open; the Player journey no longer probes eight indexes to find Button layout.

Opus 3 (screens dressed in Draw). Resolved for Draw: public rooms, rankings and replays set their eyebrow, chips, pane width, tabs, toolbar grid, refresh hint, cards, detail and the opening-row focus in their own Grid, Dress, Card and Detail, and Draw's only remaining screen-specific lines are the welcome, post-match and report branches. On UpdateStatus, I accept the author's reason for the room lines: the `room` and `room-chat` lines are fallbacks under `status.empty()` and the `room-table` line assigns unconditionally, and owner->Status runs after the notice and pad-hint lines, so moving them there would let a table's seat line beat a notice. That does not hold for the `languageSaveError_` line, which sits immediately before owner->Status; see finding 3.

Opus 4 (3P tab glyph). Resolved. ShortcutButtons drops `lightPunch & heavyPunch` from a stick's sets, so input and glyphs agree; menu_buttons_test now expects no tab glyph when only 3P is left, and a new case shows LP and HP still step and are named beside a 3P button, with LP and HP together cancelling.

Sol 1 (tab replacement). Resolved. MenuNavigation::Replace(screen, depth) cancels the modal, pops `depth` screens (never the root) and pushes; the shell and DrawTrainingPanel both use it. I traced the shell's depth for tab to tab, panel to tab, tab to panel and panel to panel, and Training's default depth 1 from its root and from a page; all are right. shell_training_journey_test opens a player's frame data reader, clicks the strip's next tab, and one Back reaches the lab.

Sol 2 (hint clicks). Resolved in substance. DrawHintBar records the hint's press in hintPress_, and ReadFrame ORs it into the next frame's held buttons ahead of the notice, editor, toast and navigation readers, so a hint is answered exactly as its button is; the bespoke Pick/Choose/Adjust/TabAction dispatch is deleted. Shortcut, tab and bell hints are left out while a dialog owns the input. HintPresses covers Fighter, Select opening a value panel, no shortcut while a choice is open, Select picking the highlighted option, and a confirmation not answered from the bar. One cosmetic residual, finding 4.

Sol 4 (deciding Arcade prompt). Resolved. Character select is always a row, the button row's width is computed from the row count, and DecidingPromptOnStick drives a DirectInput stick with no Fighter or Options glyph to Character select with the D-pad and Select, then to fighter select once the game is over.

Sol smaller items. Resolved. DirectionRepeat is one class used by the navigation (choices, questions, lists) and, through dialogRepeat_, by the notice and the on-screen keyboard, reset when neither reads; DialogRepeat covers both. PressKey keeps every draft copy in a WipedText, Shift returns before any copy, and Done returns before the draft is copied at all. Draw now ends every screen, embedded ones included, with AnswerToast and DrawToasts, and DrawScreen's `false` result keeps a toast from taking focus over an embedded screen; a journey covers a toast said over fighter select.

## Findings, in priority order

### 1. HEAD's SettingsStore test fails: it was committed with the crash-dump task's key rename, and the rename was not

src/tests/settings_store_test.cxx:BoolPreferenceRoundTrips and main were changed in 2e5493dbe to expect the key `sendProblemReportsWithDump` (two hunks). At HEAD, src/netplay/BoolPreferences.hxx line 27 still maps `sendProblemReports`; the rename to `sendProblemReportsWithDump`, with its PlayerPreferences comment, exists only in the uncommitted working tree of the crash-dump task. BoolPreferenceRoundTrips looks each expected key up in BoolPreferences and CHECKs it is found, so a clean build of HEAD fails SettingsStore. The local SettingsStoreTest.exe passes only because it was built at 22:42 from the working tree, after BoolPreferences.hxx changed at 22:37. The commit also has nothing to do with problem reports, so the hunks are scope bleed as well as a red test.

Remedy: take the two settings_store_test.cxx hunks out of this branch (amend or a follow-up commit restoring `sendProblemReports` in both places), and let the crash-dump change commit them together with the BoolPreferences.hxx and PlayerPreferences.hxx rename. Before handing off, build a clean worktree of the branch tip and run SettingsStore there.

### 2. controller_navigation_test.cxx crosses 1000 lines in this branch

src/tests/controller_navigation_test.cxx goes from 892 lines at fcd33f73 to 953 at round 2 and 1032 at HEAD, the last step from HintPresses and DialogRepeat. The standard treats this as a presumptive blocker, and the file already holds two separable groups: the dialog contract journeys (DialogContract, HintPresses and DialogRepeat, lines 694 to 924) and the training pad journeys (PracticeFightPresses through GoNowBehind, lines 260 to 688). The build already links several sources into one test target (ShellJourney, IdentityJourney, PublicRoomsJourney in cmake/sf4e_test_targets.cmake).

Remedy: move DialogContract, HintPresses and DialogRepeat into src/tests/menu_dialog_test.cxx, declare the three in a short header or as externs, add the file to the ControllerNavigation SOURCES line, and keep main's call order. That leaves controller_navigation_test.cxx near 800 lines with no new ctest entry.

### 3. Opus 3 residuals that can move without changing precedence (not blocking)

ApplicationShell.cxx:UpdateStatus applies `languageSaveError_` for `SettingsTabOf(screen)=="tab-access"` on the line directly before `owner->Status(...)`, and the owner of every `tab-access` screen is SettingsScreens. Moving that line to the start of SettingsScreens::Status changes no outcome and takes the last Settings-specific status out of the shell. ApplicationShell.cxx:BuildRows still carries the `tournament-matches` focus fix-up (`identity_.TakeFocus`) after IdentityScreens' own rows; it belongs in IdentityScreens' Rows or Dress. Round 2 called these owed rather than blocking, and they stay so.

### 4. Under a confirmation, the bar's Select and Back look clickable and are not (low priority)

GameMenuDraw.cxx:DrawHintBar still emits the dialog's Select and Back as interactive hints while a confirmation, choice popup, notice or editor is open, and those popups are ImGui modals that take the pointer, so the click is silently eaten; HintPresses asserts exactly that. The comment documents it, but Sol's remedy was either to make the hints inert labels there or to host them in the dialog. Remedy: when the open dialog is a popup modal (Confirming, a choice drawn as a popup, a notice, the editor), draw the hints through v2::HintBar's non-interactive style, or skip the hit test, so the bar never looks pressable where it is not. A board's value panel, which is not a popup, keeps clickable hints.

### 5. Background now saves more than room settings while Ember is hidden (confirm intent)

ApplicationShell.cxx:Background used to call UpdatePreferenceSave only when the dirty edit was room settings alone. It now calls it every hidden frame, and MaySave passes on canEditPreferences alone. Two behaviours change: a non-room setting left dirty when the player hides Ember at an idle main menu is saved while hidden instead of when Ember next shows, and a queued save's acknowledgment (the profile-saved notice and the Return from `main-character`) is processed while hidden. Both look like improvements and the deletion is the simpler shape, but neither the commit message nor a test states it. Remedy: say so in the commit message or the Background comment, and add a line to an existing save journey (hide with a pending non-room edit, check the SavePreferences action) so the behaviour is pinned.

Nit: in ApplicationShell.cxx the comment "The room's own status line: the kind of room it is, and whether it is locked." now sits above MaySave, with MaySave's own comment below it; move it back above RoomStatusKey.

## What unblocks approval

Finding 1 is a correctness problem in committed content: the branch tip fails its own test suite on a clean build. Finding 2 is the standard's file-size rule and a mechanical split. Fixing those two is enough for my approval; 3 to 5 can follow. Everything round 2 asked for is in place and checks out against the code.

VERDICT: CHANGES REQUIRED
