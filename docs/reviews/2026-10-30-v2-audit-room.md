# V2 input audit: room board, Arcade, post-match and the screens around them

Snapshot: the Nightly branch at fcd33f73 (read-only copy in sf4-nightly-audit).
Scope: the V2 look only. Anything inside a `!v2::On()` branch is ignored, but
rows that the Classic-era builders still push and that reach the V2 board are
in scope. Findings name a file and a function, never a line, because the
Classic look is being deleted in parallel.

Method: every finding was traced through the code that builds the rows, the
code that draws them (PressEntry, the board bodies), MenuNavigation::Update and
the action dispatch. "Unverified" marks what could not be traced to the end
(mostly runtime or in-game behaviour). Nothing here was run in the game.

Severity: P1 blocks V2 becoming the only look; P2 should be fixed; P3 polish.

## Summary

| # | Sev | Title |
| --- | --- | --- |
| 1 | P1 | Arcade board: Left is a dead end and the action bar is reached only by walking every row (Discord report 1) |
| 2 | P1 | Two invisible focus stops (members-part, chat-part) on every V2 board |
| 3 | P1 | On a short or scaled screen the Classic "More" row leaks into V2 as an invisible modal and hides Room settings and the invitation copies |
| 4 | P1 | Arcade post-match prompt: no answer or Back to room stands the player up; Rematch sends a Ready the room refuses (Discord report 2) |
| 5 | P1 | V2 Select silently steps word values on room screens (Input delay Auto becomes 0 frames, Admission Locked becomes Open) |
| 6 | P2 | Classic-type V2 table cards move in list order, not as the grid they are drawn in |
| 7 | P2 | Up on a top-row cabinet moves one cabinet left |
| 8 | P2 | Post-match V2: Down moves the focus onto the undrawn player rows |
| 9 | P2 | Post-match V2 in King of the hill after a set: buttons wear the wrong glyphs and the third button is the player's name |
| 10 | P2 | Post-match: a mashed Back (B, or MK on a stick) at the WIN screen gives up the seat |
| 11 | P2 | Narrow room tabs draw LB and RB caps that the pad cannot press |
| 12 | P2 | V2 hint bar shows X, Y and View shortcuts on devices that do not have them |
| 13 | P2 | Button layout capture: the hint bar offers "B Cancel", but B is bound instead |
| 14 | P2 | A Discord invitation pushes its screen over the chat box mid-typing; the rest of the typing becomes menu presses |
| 15 | P2 | Chat box says "Type a message" to a pad player who cannot type there, though V2 has an on-screen keyboard |
| 16 | P2 | Settings, Interface, Room layout rows are disabled inside a room, where the grips and More… still change the same settings |
| 17 | P2 | Arcade cabinets drop the table banner: held-start countdown, Ready timeout and the Training call are invisible on the board |
| 18 | P2 | Arcade rows re-centre the focused cabinet every frame, so the mouse wheel cannot scroll them |
| 19-36 | P3 | Polish, listed at the end |

## Discord report 1: "left should go to the left side of the lobby"

### How the Arcade board's neighbours are wired today

Where: RoomPanel.cxx, ApplicationShell::RoomBoardRows, the block after the V2
rebuild that sets `e.left/right/up/down` (memberRight, afterCabinets,
belowCabinets, below); MenuNavigation.hxx, MenuNavigation::Update for every
direction left empty.

The V2 rows end up in this order: cabinet-0 to cabinet-15, one matchup chip per
pair, the shown members (grouped), room-members (the Members heading),
room-hide-members (its Hide button), room-chat (the Type a message box),
room-hide-chat (the chat heading's Hide), members-part, chat-part (see finding
2), room-hide-picture when a picture is on the board, ready (when seated),
room-training, copy and copy-short (one slot, private rooms), room-more, the
asking rows (end-practice, stop-watching, replace-room) and leave.

The wiring:

- Cabinet c: Left is c-1 inside its row of eight, but itself on cabinets 0 and
  8 (a dead end). Right is c+1, and past the row's end the first member (or
  the Members heading). Down is c+8 from the top row, and from the bottom row
  `below`: the first matchup chip, or with no pairs the first row that is not a
  cabinet, chip or member, which is the Members heading at the top of the
  right column. Up is set only on the bottom row; see finding 7 for the top.
- Matchup chips: Right is the first member; Left does nothing; Up and Down walk
  the list, so Down from a chip goes to the next chip (beside it on screen, the
  strip is four across) and from the last chip to the first member at the top
  right; Up from the first chip goes to cabinet 16.
- Members: Left is pinned to the player's own cabinet (cabinet 1 when not
  seated); Right does nothing; Up and Down walk the list, so Down from the last
  member goes up to the Members heading above the list.
- Everything else (headings, the chat box, the whole action bar): Left and
  Right are the previous and next row in the list, and Up and Down walk the list
  too. Up from Ready therefore goes to chat-part, an undrawn row, not to the
  cabinets above it; Left from Ready does the same.

Nothing ever moves from the tables to the action bar except Down through the
chips, and nothing moves from the right column to the action bar except walking
every row in between.

### Press counts to Leave room

Room for the counts: private Arcade room, wide board (1920x1080 at 1.0x, so
the layout is Wide, not WideShort), members and chat shown, no picture, the
player seated and paired. "Cabinet 7" is the seventh cabinet on screen
(cabinet-6, top row). Shortest route in each case.

| Start | 4 members, 2 pairs | 16 members, 8 pairs |
| --- | --- | --- |
| Cabinet 7, by Right then Down | 17 | 29 |
| Cabinet 7, by Down only | 19 | 37 |
| First member, by Down | 15 | 27 |
| Any member, Left first | Left lands on the player's own cabinet, then as above | same |

The 17: Right to cabinet 8, Right to the first member, three Downs to the last
member, then Members heading, Hide, chat box, chat Hide, members-part,
chat-part, Ready, Training, Copy invite, Copy link, More…, Leave room. In
general, from a top-row cabinet k (0-based, k = c mod 8) it is M + 19 - k by
the Right route and P + M + 13 by Down, where M is the member count and P the
pair count; from member i it is (M - i) + 12. Holding Down repeats after 0.35 s
and then every 0.085 s, so the player holds for about two seconds and lands on
Leave room because it is the last row, which is exactly the report.

Narrow board (under 820 logical pixels, where the layout is also Stacked): the
same walk minus chat-part, so 16, 18 and 14 for the first table above. The tab strip follows the focus: Board for the cabinets, Members for the
member rows and the Members heading, Chat for the chat box, and back to Board
for members-part (no focus mark anywhere, see finding 2) and the action bar. The
player watches the screen flip Board, Members, Chat, Board.

Between 820 and 860 logical pixels the board is narrow but chat-part is still
added, so it is one press more (see finding 26).

### The Classic-type V2 board

Where: same function; table rows get only `e.right = memberRight`.

The cards are drawn two across (three at 1640 logical or wider, one on a narrow
board under 640 px of tables). Right from table 1 jumps to the first member,
skipping table 2 beside it; Down from table 1 goes to table 2 (to its right);
Left does nothing; members' Left goes to the last focused table. With four
tables and four members, table 1 to Leave room is 16 presses (Right, three
Downs, then the same twelve as above) or 19 by Down. See finding 6.

### Proposed fix: one neighbour rule from the geometry

Every board row already passes its box to PressEntry (the kit's rule 2), so
the layout knows where each row is. Use that instead of id strings.

1. PressEntry records `{id, box}` for the row it was given, in screen
   coordinates, into the navigation (for example `MenuNavigation::Place(id,
   rect)` with a plain float rect, so MenuNavigation keeps no ImGui
   dependency). GameMenu::Draw swaps "this frame's places" to "last frame's"
   once per frame and clears them when the screen changes.
2. In MenuNavigation::Update, after an explicit `e.left/right/up/down` (kept for
   any screen that truly needs one) and after the Adjust of a value row, when
   last frame placed the focused row: pick the placed row whose centre lies past
   the focused centre in the pressed direction, scored by the gap along that
   axis plus twice the offset across it (zero when the boxes overlap on the
   cross axis), ties by list order. No candidate means no move: the screen's
   edge is the only dead end. With no places (a plain list, the first frame of a
   screen) the current list movement stays.
3. Rows inside scrolling children (the arcade rows, members, the table cards)
   are placed where ImGui puts their invisible buttons, scrolled or clipped, so
   off-screen cabinets still count and SetScrollHereY brings them in once
   focused.
4. The frame N boxes feed frame N+1's input. The board's layout does not move
   between two frames of one press, so the lag is invisible.
5. Narrow board: the hidden parts are not drawn and so not placed. Either let LB
   and RB step the three tabs (finding 11) and keep geometry inside the shown
   part, or have DrawRoomBoard place the hidden parts' rows as virtual boxes one
   area-width to the right (Members) and two (Chat), so Right off the board's
   edge lands on the Members tab, which the tab strip already follows. The
   first is simpler and matches the tab model.

Then delete, in ApplicationShell::RoomBoardRows: memberRight, afterCabinets,
belowCabinets, below, the cabinet, chip, table and member neighbour rules, and
the prev/next wiring of every other row. A row that is not drawn can no longer
be reached by the pad, so mouse and pad reach exactly the same rows (findings 2
and 3 become impossible by construction).

What the geometry gives on the Arcade wide board without any special case:
Left on cabinets 1 and 9 stops at the screen edge (there is nothing to the
left); Right from cabinet 8 or 16 reaches the member or chat row level with it;
Left from a member reaches the cabinet in the same band; Up on the top row stops
(or reaches the King of the hill banner if it ever becomes a row); Down from the
bottom row reaches the chip under it, then the action-bar button under that;
Up from Leave room reaches the chip or cabinet above it; Left from the chat box
reaches Leave room, which sits level with it; Down from the last member reaches
the chat heading's Hide and then the chat box, never the Members heading above
the list. Cabinet 7 to Leave room becomes Down, Down (chip), Down (More…, the
button under cabinet 7), Right: four presses, three without pairs. The first
member to Leave room becomes Left, then the same, or Down through the members,
Down to the chat box, Left. The Classic-type cards navigate as the grid they
are drawn in, and the action bar's second line (when it wraps) is reached by Down,
not by Right past the end of the first line.

Fallback if spatial focus is not wanted in MenuNavigation: one function,
`RoomBoardNeighbours(layout, rows)`, fed by the same boxes ArrangeRoom and
DrawArcadeBoard compute (tables grid with its columns, chip strip, action-bar
lines, members column, chat column), returning the whole map in one place. It
is more code and repeats the layout's knowledge, so the geometric rule is
preferred.

## Discord report 2: the rematch prompt and the Arcade Rematch choice

What was traced: ApplicationShell__PostMatch.cxx (UpdatePostMatch,
DrawPostMatch, ClosePostMatch), RoomPanel.cxx (RoomBoardRows' cabinet rows),
RoomActions.cxx (ApplicationShell::RoomAction's Rematch and New opponent),
RoomModelArcade.cxx (ArcadeSetEnded, ApplyRematch), RoomModelActions.cxx
(ApplyReadiness), sf4e__NetplayRuntime__Room.cxx (the Ready and Rematch
dispatch), PressGate.hxx.

- Focus: the prompt opens with `nav.Prefer("again")`. When Rematch is disabled
  (queue waiting) the focus still lands on it and A does nothing (finding 31).
  In King of the hill after a set there is no "again" row and the focus falls to
  Character select.
- Held button from the fight: the prompt is drawn on a fresh screen
  (MenuNavigation::Push and GameMenu::Draw both Rearm the PressGate), so a
  button held through the WIN counts only once released. That is correct. A
  button pressed again after the prompt opens counts at once, which is how
  players mash through WIN screens; see finding 10.
- Timeout: 10 seconds (PostMatchSeconds); after a Rematch the prompt waits a
  further 15 seconds for the opponent (StartGraceSeconds). No answer leaves the
  seat after a set or with no set length, unless KeepsSeat says otherwise;
  KeepsSeat knows King of the hill only (finding 4).
- The Arcade cabinet's own Rematch / New opponent: after a set
  (`table.deciding`) the player's own cabinet row carries two choices, opened by
  A as a value panel at the cabinet (bodyChoices is set by DrawArcadeBoard).
  Rematch is disabled once chosen. B on the cabinet still leaves the seat at
  once (no cost question, since the score was reset when the set ended). This
  part works, but it is a second, separate place to answer the same question
  the post-match prompt asked a moment earlier, and the two disagree (finding 4).

## Findings

### 1. Arcade board: Left dead end, action bar only by walking every row (P1)

Where: RoomPanel.cxx, ApplicationShell::RoomBoardRows (neighbour block);
MenuNavigation.hxx, MenuNavigation::Update.
Repro: pad or keyboard, wide or narrow V2 Arcade room, seated. From any cabinet
or member try to reach Table options (More…) or Leave room.
What happens: see the section above; 15 to 37 presses, Left from cabinets 1 and
9 does nothing, Left from members always returns to the player's own cabinet.
Expected: the regions behave as they are drawn: Left goes left, Down from the
cabinets reaches the action bar under them.
Fix: the geometric neighbour rule above, deleting the id-prefix wiring.
Verified by code trace.

### 2. Two invisible focus stops on every V2 board (P1)

Where: RoomPanel.cxx, ApplicationShell::RoomBoardRows pushes members-part and
(unless Stacked) chat-part with no V2 gate, and the V2 rebuild keeps them;
ApplicationShell::DrawRoomBoard draws them only in the Classic wide branch.
RoomActions.cxx, ApplicationShell::RoomAction flips roomMembersHidden or
roomChatHidden for them.
Repro: pad, V2 room, Down from the chat box (or from the chat Hide button).
What happens: two presses land on rows that are not drawn: no focus mark, the
explanation line reads the Classic "part" detail, and on the narrow board the
tab strip jumps to Board with nothing focused. A here flips a Classic preference
V2 ignores and saves it. Left from Ready lands on chat-part too.
Expected: every focusable row is drawn and clickable.
Fix: build these rows only where they are drawn (with Classic going, delete
them and their RoomAction branch); the geometric rule makes undrawn rows
unreachable anyway. Verified.

### 3. Short or scaled screens: the Classic "More" row leaks into V2 as an invisible modal (P1)

Where: RoomPanel.cxx, ApplicationShell::RoomBoardRows (`share` folds
room-admin, copy and copy-short into a row with id "more" when boardLayout_ is
WideShort; the V2 rebuild sends "more" to the kept rows because no branch names
it); ApplicationShell::DrawRoomBoard sets boardLayout_ from
`DisplaySize.y < 820 * s` in V2 too, and sets bodyChoices only for popout rows;
GameMenuV2.cxx, GameMenu::DrawV2 (Values draws nothing without bodyChoices) and
GameMenu::Finish (no DrawChoiceModal for a body screen).
Repro: V2, 1280x720 or 1280x800 at 1.0x (Steam Deck), 1920x1080 at 1.5x, or TV
mode on 720p. Open a room.
What happens: the action bar shows two More buttons (the Classic "More" and
More…); Copy invite and Copy link are gone from the bar; Room settings is
missing from More…. A on the Classic "More" opens a choice that is never drawn:
the hint bar says Choose and Cancel, Up and Down move an invisible highlight, A
picks an unseen option, B closes it.
Expected: the same bar at every size, every choice visible.
Fix: compute boardLayout_ only for the Classic board (or delete it with
Classic) so `share` never folds in V2; add a test that every room-board row with
choices draws a panel. Verified by code trace; the 1280x800 V2 case is in
room_panel_navigation_test's screen list but only clicks the seat chooser.

### 4. Arcade post-match: no answer stands the player up, Rematch is refused (P1)

Where: ApplicationShell__PostMatch.cxx, KeepsSeat, UpdatePostMatch,
DrawPostMatch; RoomModelArcade.cxx, ArcadeSetEnded; RoomModelActions.cxx,
ApplyReadiness.
Repro: Arcade room (not King of the hill). Finish a set (with no set length,
every game ends one; inside a longer set the seat is kept). At the prompt do
nothing for 10 s, or press B (Back to room); separately, press Rematch.
What happens: KeepsSeat is true only for King of the hill or mid-set, so no
answer and Back to room both set AfterPostMatch::LeaveSeat, and once the game is
back at the main menu LeavePlace sends Unqueue: the player is stood up from the
cabinet, which the Arcade design says never happens after a set. Rematch sends
CommandKind::Rematch, which the runtime turns into a Ready; the room has set
`deciding` and ApplyReadiness rejects Ready while deciding. The prompt then
shows "waiting for" the opponent for up to 25 s and closes, and the real
Rematch / New opponent question is still open on the cabinet.
Expected: in an Arcade room the prompt asks Rematch or New opponent and sends
ActionKind::Rematch or NewOpponent; no answer keeps the cabinet.
Fix: make the prompt's "after a set" rule the room type's (the room model
already knows: a deciding table keeps both seats), so KeepsSeat reads the
table's deciding state instead of naming KingRoom; send the table's Rematch
action when the table is deciding. Verified by code trace up to the room's
rejection; whether the rejection is shown to the player is unverified.

### 5. V2 Select silently steps word values on room screens (P1)

Where: MenuNavigation.hxx, MenuNavigation::Choose (the `selectSteps` case);
GameMenu.cxx, GameMenu::Draw sets selectSteps for every V2 screen;
ApplicationShell.cxx skips OfferValues on every screen whose id starts with
"room"; RoomPanel.cxx, RoomTableRows (input-delay) and RoomAdminRows (lock);
RoomActions.cxx, RoomAction.
Repro: pad, V2, Auto input delay on. Seated at a table, Y for Table options,
focus Input delay (value "Auto" or "Auto (2 f)"), press A. As host, Room
settings, Admission "Locked", press A.
What happens: the row has no choices and does not open, so Select becomes
Adjust +1: Input delay leaves Auto for 0 frames (which Auto and the
recommendation never choose) and is sent at once; Admission unlocks the room;
Edition Select can be turned on but never off; Set length and After a set step
forward. The hint bar shows no Select hint for these rows, so nothing warns.
Expected: Select does what the hint bar says, or nothing.
Fix: remove the selectSteps special case; a value row that should open its
values on Select gets them as choices (OfferValues), on room screens too.
Verified by code trace.

### 6. Classic-type V2 cards move in list order, not as a grid (P2)

Where: RoomPanel.cxx, ApplicationShell::RoomBoardRows (table rows) and
DrawRoomBoard (the "V2 tables" child, two or three across).
Repro: pad, V2 Classic room with three or four tables.
What happens: Right on table 1 goes to the members, skipping table 2; Down on
table 1 goes to table 2 to its right; Left does nothing.
Expected: grid movement, and Right off the last column to the members.
Fix: the geometric rule. Verified.

### 7. Up on a top-row cabinet moves one cabinet left (P2)

Where: RoomPanel.cxx, ApplicationShell::RoomBoardRows (up is set only for
cabinets 8 to 15); MenuNavigation::Update (list movement for an empty up).
Repro: pad, Arcade room, focus cabinet 4, press Up.
What happens: focus moves to cabinet 3; Up on cabinet 1 does nothing.
Expected: nothing above, so no move.
Fix: the geometric rule. Verified.

### 8. Post-match V2: Down moves the focus onto the undrawn player rows (P2)

Where: ApplicationShell__PostMatch.cxx, DrawPostMatch (rows again, fighter,
back, pm-you, pm-opponent, drawn with three columns in V2; the body draws only
the first three as buttons); MenuNavigation::Update grid movement.
Repro: pad, V2, the rematch prompt, press Down on any button.
What happens: Down from Rematch lands on pm-you, from the other two on
pm-opponent: info rows drawn as the player tags, with no focus mark and no
Select hint. Up returns.
Expected: Down does nothing (the buttons are the only row).
Fix: do not make the player lines rows (they are pictures of state, the speech
can read them from the title and detail), or the geometric rule with them
unplaced. Verified.

### 9. Post-match V2, King of the hill after a set: wrong glyphs and a name as a button (P2)

Where: ApplicationShell__PostMatch.cxx, DrawPostMatch (the V2 body draws
`entries[0..2]` with glyphs Select, X, Back by position).
Repro: King of the hill, finish a set, V2.
What happens: with no "again" row the three buttons are Character select with
the A glyph, Back to room with the X glyph (X is Character select), and the
local player's name with the B glyph.
Expected: the buttons are the actions that exist, each with its own glyph.
Fix: draw the buttons from the rows that are actions (by id), and take each
glyph from what the button does. Verified.

### 10. Post-match: a mashed Back gives up the seat (P2, in-game unverified)

Where: ApplicationShell__PostMatch.cxx, DrawPostMatch (`back`), UpdatePostMatch;
PressGate.hxx.
Repro: Classic-type room with no set length, Xbox pad or a DirectInput stick
(Back is MK). Mash B or MK through the WIN screen.
What happens: PressGate blocks only what was held as the prompt opened; the
next press counts. B is Back to room, which after a game with no set length
leaves the seat for whoever waits. A mashed A or LK is Rematch, which is
harmless.
Expected: a prompt that appears under mashing ignores presses for a moment, or
Back to room asks nothing destructive.
Fix: arm the post-match screen only after a short neutral period (a PressGate
"armed after all buttons up for N ms"), shared by every prompt that appears
over a fight. Verified by code trace; whether players actually mash MK there is
unverified.

### 11. Narrow room tabs draw LB and RB caps the pad cannot press (P2)

Where: RoomPanel.cxx, ApplicationShell::DrawRoomBoard calls v2::Tabs for Board,
Members and Chat; V2.cxx, v2::Tabs draws LB and RB caps; GameMenu.cxx,
GameMenu::Draw turns LB and RB into nothing when menu_.tabs is empty.
Repro: narrow V2 room board (for example 1280x720 at 1.5x), press RB.
What happens: nothing; the caps are only clickable with the mouse.
Expected: RB to Members, LB back, as everywhere else that shows the caps.
Fix: let the board's tab strip answer LB and RB through the same path as
GameMenu::tabs (one tab model), or stop drawing caps on strips nobody steps.
Verified.

### 12. V2 hint bar shows X, Y and View where the device has no such buttons (P2)

Where: GameMenuV2.cxx, GameMenu::DrawV2 (hint bar adds every shortcutHints
entry); the Classic legend in GameMenu::Draw filters them to an XInput pad or
the keyboard and drops a glyph the player made Select or Back.
Repro: DirectInput stick, V2, any room screen, Public rooms (Y Refresh), or the
post-match prompt (X Character select, drawn on the button too).
What happens: hints and a button glyph name buttons the stick does not have.
Expected: the same filter as before.
Fix: move the filter into the shortcut hints' builder so both looks (soon one)
read the filtered list. Verified.

### 13. Button layout capture offers "B Cancel" (P2)

Where: ApplicationShell__ButtonLayout.cxx, ApplicationShell::DrawButtonLayout
(backHint Cancel while capturing, listEnabled false); GameMenuV2.cxx,
GameMenu::DrawV2 adds the Back hint whenever backInert is false;
ButtonLayoutCapture.hxx (cancels are Start, a direction, Escape, Back from the
hint, Timeout and State).
Repro: pad, V2, Controllers, Button layout, Light punch, then read the hint
bar and press B.
What happens: the bar says "[B] Cancel"; B is captured and bound to Light punch.
A click on the hint does cancel.
Expected: the bar names Start (and a direction) for the pad.
Fix: while listEnabled is false the hint bar shows no Select or Back glyph,
only the capture's own hints. Pad disconnect mid-capture cancels correctly
(LayoutCancel::State through `editable`). Verified: LayoutCapture::Read cancels
only on Start or a direction and takes any other listed button, B included.

### 14. Discord invitation pushes its screen over the chat box mid-typing (P2)

Where: ApplicationShell.cxx, the frame update that runs `nav.Push
("discord-invitation")` and Say(...) with Join and Later when a Discord
invitation arrives; ApplicationShell__Toasts.cxx, DrawToasts and AnswerToast;
GameMenu.cxx, KeyboardMenuBits (Enter is Select even while typing) and
GameMenu::Draw (toastFocused only excludes the Edit modal, and the chat box is
a raw InputText).
Repro: V2, in a room, Chat screen, typing; a Discord invitation arrives.
What happens: the screen changes at once to the invitation screen; the chat
draft survives in chat_, but the box is gone, typesText is off, and the rest of
the typing is menu keys: F opens Change fighter, T Table options, C Chat, Space
presses the focused row (Cancel invitation, or after Join the Switch question).
The toast also holds focus, so the first Enter or Space is its Join. Any
Edit modal open elsewhere (room name, template name) is cancelled by the Push.
Expected: an arriving invitation shows as the toast and waits; nothing moves
the screen under the player.
Fix: drop the Push and let the toast's Join open the screen (it already
carries the screen and row); keep the toast from taking focus while a text box
holds the keyboard (typesText). Verified by code trace.

### 15. "Type a message" to a pad player who cannot type (P2)

Where: RoomPanel.cxx, ApplicationShell::RoomChatRows (compose is a plain Row,
not a text row); ChatPanel.cxx, ApplicationShell::DrawChatScreen and
DrawChatBox; GameMenu's on-screen keyboard opens only for text rows.
Repro: pad only, V2, room board, select "Type a message".
What happens: the Chat screen opens with a box only a physical keyboard can
fill; A on it is a disabled Send. CONTROLLER_MENUS.md says typing needs a
keyboard, but the V2 board's box invites it and V2 has an on-screen keyboard.
Expected: the pad gets the on-screen keyboard, Done sends.
Fix: make compose a text row whose acceptance sends (the keyboard's Recent row
would then hold chat lines, which is fine). Verified; design intent unverified.

### 16. Room layout rows are disabled inside a room (P2)

Where: ApplicationShell.cxx, the Interface rows arrange-column, arrange-split,
arrange-preset, arrange-members, arrange-chat, arrange-picture, arrange-reset
(enabled on canEditPreferences, which the runtime clears whenever a room is
joined); ApplicationShell::UpdatePreferenceSave saves room-layout changes in a
room (OnlyRoomSettingsChanged); ApplicationShell::RoomGrips and More… write
the same preferences without a check.
Repro: pad, in a room, Settings, Interface, Room layout.
What happens: every row is disabled with "leave the room", while the mouse can
drag the grips on the board behind it and More… flips the parts.
Expected: the documented pad alternative to the grips works where the grips do.
Fix: one rule for "can change the room layout now" used by the rows, the grips
and the save. Verified.

### 17. Arcade cabinets drop the table banner (P2)

Where: RoomPanelArcade.cxx, ApplicationShell::DrawArcadeBoard (the Arcade and
King branches never call DescribeTableBanner); RoomControls.cxx, CabinetState;
RoomPanel.cxx, RoomBoardRows (the own cabinet's detail is not replaced during
a held start, unlike a table card's).
Repro: Arcade room with a locked-in spectator holding the start, or one fighter
readied with the Ready timeout running, or the opponent in Training.
What happens: nothing on the board shows the countdown or the reason; the
player sees it only in Table options (Y).
Expected: the cabinet's screen or its detail carries the banner, as a table
card does.
Fix: put DescribeTableBanner's text on the cabinet's screen line (the paired
cabinet's second line) and in its row detail. Verified.

### 18. Arcade rows fight the mouse wheel (P2)

Where: RoomPanelArcade.cxx, ApplicationShell::DrawArcadeBoard passes `scrolls`
(true whenever the rows scroll) as PressEntry's scroll argument; GameMenuV2.cxx,
PressEntry calls SetScrollHereY for the focused row whenever scroll is true.
Repro: V2, Arcade, a short window or 1.5x so the cabinets scroll; wheel over
them without moving the pointer.
What happens: every frame re-centres the focused cabinet, so the wheel has no
lasting effect.
Expected: scroll into view when the focus changes, as every other board does.
Fix: pass the focus-changed flag, as DrawRoomBoard's own rows do. Verified.

## P3 findings

19. Dead layout. RoomPanelArcade.cxx, ApplicationShell::DrawArcadeBoard's
    Classic branch (`!type`) is unreachable: DrawRoomBoard calls it only for an
    Arcade or King room. Delete it.
20. Ultra-wide boards contradict the wiring. RoomPanel.cxx, DrawRoomBoard's
    `ultra` layout puts members on the left of the tables, while RoomBoardRows
    sends Right from the last cabinet to the members and Left from a member to a
    cabinet. Solved by the geometric rule.
21. Selecting yourself in V2 opens a page of disabled rows. RoomPanel.cxx,
    RoomEntries gives no popout to the local member, so Select pushes
    room-member with Mute, Kick and Transfer disabled. Make the own row an info
    row.
22. Kick and Transfer host leave the player on the member page after Cancel.
    RoomActions.cxx, RoomAction pushes room-member to ask; Cancel closes the
    question but not the page. If the member leaves meanwhile the question is
    cancelled by Reconcile (no dead end) and the page says the member left. Ask
    the question at the popout instead.
23. More…'s Picture sub-list: B closes More… entirely instead of going back to
    its main list. RoomPanel.cxx, RoomBoardRows (morePicture_).
24. Y on a free cabinet opens the last selected table's options, not anything
    about that cabinet. RoomActions.cxx, RoomShortcut and DrawRoomBoard's
    selectedTable_ update (only occupied cabinets set it).
25. Matchup chips are focus stops where A does nothing (InfoRow). Either let A
    open that pair's table (to watch), or make them unfocusable.
26. Three narrow thresholds on one board: DrawRoomBoard's `wide` (820),
    its narrow (860 logical) and the kit's v2::Narrow (900). Between 820 and 860
    the narrow board still gets chat-part. Use v2::Narrow only.
27. The watched picture's drag state is static. WatchedPicture.cxx,
    DrawWatchedPicture keeps `moving`/`resizing` across pictures; if the game
    ends mid-drag, the next picture snaps to the pointer's corner on its first
    frame and saves it.
28. The corner picture takes the mouse over the top right, which on a narrow
    room board covers the header chips and possibly the RB cap and Chat tab.
    Unverified at exact sizes.
29. Tabs and rows disagree on Back. Rankings' "Players" row and Replays'
    tournaments row push the second tab on top of the first, while LB and RB
    replace one tab with the other (the shell's tab Shortcut returns before it
    pushes), so Back from the second tab goes to the first or to the parent
    depending on how it was reached.
30. Training's tab order and set do not match V2_MENUS.md: the code has Dummy
    recording, Input history, Frame data, HUD, Dummy, Dummy reply, Position;
    the guide lists six in another order. TrainingPanel.cxx, the trainingMenu
    tabs.
31. Post-match focus on a disabled Rematch. DrawPostMatch prefers "again" even
    when a queue disables it; prefer the first enabled button.
32. Post-match ignores the right mouse button (backInert in V2), unlike every
    other screen.
33. A public room card that vanishes on refresh hands its focus by index to
    whichever card moved into its place; the next A joins that room.
    PublicRoomsPanel.cxx, Rows; MenuNavigation::Reconcile.
34. The Join screen's invitation text row keeps invitations in the on-screen
    keyboard's Recent row for the session, visible on stream. Mark it secret or
    skip Recent for it.
35. Who is typing shows only on the Chat screen, not under the board's chat.
36. A toast that appears while a direction is held swallows the hold and the
    next press (toastGate_ treats the held button as stale and the list gets
    nothing until the toast loses focus).

Also noted, not findings: Create's two panels move only in list order (Left and
Right adjust values, so the Rules panel is reached by Down through the Room
panel), which the board's own comment intends; the public "Create" button is
disabled with "needs an Ember ID" and offers no way to the Ember ID screen from
Create; Replays' "Watch now" is a raw ImGui::Button outside PressEntry (it maps to
the row's Select, so the pad is not short of anything). The R3 flyout toggle
lives in TrainingPad (not in this file list) and was not traced.

## Checked, no finding

These were traced and behave as documented in V2:

- The seat chooser on a table card: A opens it on the card's strip, the pad
  moves along the strip and the mouse picks from it, B cancels an open chooser
  (MenuNavigation::Update returns before the board turns B into Leave seat),
  Delete and the other shortcuts are ignored while it is open, and Table options
  in it opens that table's options.
- Your own card: A readies and takes Ready back (Unready, Cancel the start
  during a held start), B and Delete leave the seat at once or ask first when
  it costs a score or hands the seat over, and the question opens on Cancel.
  The same holds on your own Arcade cabinet, where the question opens as a
  value panel at the cabinet.
- Member popouts: Mute and Steam profile act at once; Kick and Transfer host
  ask first, starting on Cancel; a member who leaves while the popout is open
  closes it through Reconcile and the focus moves to the row now in that place
  (no dead end; see finding 22 for the page left behind).
- More… opens as a popout beside the button, its switches keep it open on
  the switch, and a click outside closes it without acting (pinned by
  room_panel_navigation_test).
- The invitation copy pair shares one slot and Left and Right step between
  them.
- The column and members/chat grips drag and snap with the mouse, also in the
  Settings preview.
- The watched picture: Small at the column's top, Big over one row of cards,
  Off through More… or Hide, the corner window on a narrow board with its drag
  and resize (see findings 27 and 28 for its edges).
- Stop watching: Ember opened over a watched game lands on it (Prefer), and it
  is drawn in the action bar in the danger tone.
- Create: the Room type row steps Classic, Arcade and King of the hill; Practice
  room only for a private room; disabled rules rows keep their reason in the
  words under the panels; Stop creating asks first and its question is drawn
  over the body.
- Join, Identity, Rankings and Controllers are standard V2 lists; a passphrase
  never reaches the on-screen keyboard's Recent row.
- The pad setup legend and the capture card's countdown.

Not traced, outside the file list: the replay playback HUD's controls and the
R3 toggle of the training controls (TrainingPad).

## Missing tests (RoomPanelNavigation first)

1. `ArcadeBoardReachesActionBarByGeometry` (room_panel_navigation_test): Arcade,
   four members, two pairs, wide 1920x1080. From cabinet 7: Down, Down, Down
   lands on an action-bar button and one Right reaches Leave room; Left from
   the first member lands on a top-row cabinet; Left on cabinet 1 stays.
2. `V2BoardDrawsEveryFocusableRow`: for Wide, WideShort (1280x720), narrow
   (1280x720 at 1.5x) and ultra (3840x1080), every room-board row id seen by
   the entries probe is also reported by the card probe (ReportMenuCard), on the
   frame it is focused. Pins findings 2 and 3.
3. `V2BoardChoicesAreDrawn`: Select on every board row that has choices opens a
   panel whose card is non-empty (bodyChoices set). Pins finding 3.
4. `ArcadeTopRowUpStays` and `ClassicV2CardsMoveAsGrid`: Up on cabinet 4 keeps
   the focus; Right from table 1 reaches table 2 and Down reaches table 3.
5. `ArcadePostMatchKeepsCabinet` (shell_room_journey_test, beside the King of
   the hill case): ArcadeRoom, no answer and Back to room send no Unqueue;
   Rematch sends the table's Rematch action while it is deciding.
6. `PostMatchDownStaysOnButtons` and `KingPostMatchGlyphsMatchActions`: V2 Down
   from each button keeps the focus on a button; with no Rematch the drawn
   buttons are Character select and Back with their own glyphs.
7. `SelectNeverStepsUnhintedValue`: V2, room-table with Auto delay and
   room-admin Locked; A on Input delay and on Admission sends nothing and
   changes nothing.
8. `NarrowRoomTabsAnswerLbRb`: narrow V2 board, RB focuses the first member, LB
   returns to the board.
9. `InvitationDoesNotInterruptChatTyping` (shell_chat_journey_test): with a
   draft on the Chat screen, a Discord invitation arriving keeps the screen;
   Enter sends the chat line, not the toast's Join.
10. `RoomLayoutRowsWorkInRoom` and `CaptureHintHasNoBackGlyph`
    (shell_controller_journey_test): arrange rows are enabled in a room and
    save; during a layout capture the V2 hint bar offers no pad Back.
