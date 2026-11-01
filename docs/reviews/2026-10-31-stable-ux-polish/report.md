# Stable UX polish, 2026-10-31

The last UI pass before the next Stable, on nightly e686ab3c (HEAD moved to
cb26bed8 during the pass, which changed nothing under `src/ui` or `locales`).
Every screen UiLab can show was shot at 1280x720, 1920x1080, 2560x1440,
640x720 and 1280x720 at 150% text, with the lab's `--check` probe on (text cut
short, text taller than its row, cards overlapping). The shots were read, then
what a player sees first was fixed.

## Fixed

1. **Training lab, 1280x720 and any size where its eight cards do not fit one
   column.** The two-column layout drew five cards: cards after the right
   column's first went back to the left column, on top of Input history,
   Frame data and HUD, which could not be seen or clicked. A new ImGui line
   resets x to the window's indent; the right column now indents. A UiRender
   check fails if any two Training home cards overlap.
2. **Arcade and King of the hill board, 640x720 and 150% text.** The action
   bar stayed fixed at the bottom, so the cabinets got what was left: at
   1280x720 150% one row of cabinets, marquee tops cut, the matchups out of
   sight; at 640x720 a stray vertical scrollbar. Now, as in Classic rooms, the
   bar follows the floor in its scroll whenever the whole floor at its least
   size does not fit over it (`ArcadeBoardHeight` measures the floor as it is
   drawn). The scroll follows the pad focus to cabinets and matchups, and rows
   that scroll sideways leave room for their scrollbar.
3. **Members panel, Arcade and King.** Status read "Playing / Slot 2" (the
   hidden table index); it now reads "Playing / Cabinet 3"
   (`room.board_status_cabinet`, `room.board_status_host_cabinet`). Classic
   keeps "Slot".
4. **Members panel at 1280x720, set records.** "Seated / Slot 1 · 2 W / 2 L"
   plus a ping was too wide for the column, and the tag drops the whole status
   chip when it does not fit. The record is its own card field and joins the
   status on the board only where both fit (`v2::TagWidth`); the Members page
   always shows it.
5. **King of the hill status line** said "Arcade room / invitation only"; it
   now says "King of the hill", with a locked variant (`room.king_status`,
   `room.king_locked_status`). A journey checks the Arcade, King and locked
   King lines.
6. **Post-match prompt, 640x720 and long locales.** Equal-width buttons cut
   "Character select"; the buttons now share their bar with
   `v2::BarLines`/`BarPlaces`, as the room bar does.
7. **Arcade cabinets at 1280x720.** Name plates read "Dec...", "FRa...". Small
   cabinets give the name the flag's room; the flag stays in Members and at
   1080p and up.
8. **Settings panels that scroll.** The scrollbar covered the focused row's
   right edge; it now sits in the section's right margin.
9. **Toasts with buttons** showed keys as text pills ("A", "Esc"); they now
   use the hint bar's prompt glyphs.
10. **Rankings at 1280x720.** "On the board as" and the board rows sat 4 px
    left of the others; all rows are 16 px in, and a long name elides beside
    its label.
11. **Rankings standing card, 640x720 at 150%.** The class line ran off the
    card in nine locales. On a narrow card the emblem takes at most a fifth of
    the width, the class name shrinks to 12 px then wraps, and the detail
    elides or drops.
12. **Notifications, empty.** "Nothing yet." was a disabled row, so the pane
    said "Unavailable"; it is now an info row, as Sent reports' is.
13. **Replays, Export video tab.** The title keeps "Replays", with the tab in
    the eyebrow, as the other tabs do.
14. **UiLab.** Sample data has an installed version ("Ember 1.5.0 · Stable")
    and six members with set records.

## Left as it is

- Cabinets at 1280x720 and 1280x800 no longer show flags so names fit; a
  visible change at the main size, for Kate.
- At 150% on 720p, Ready/Rematch sits below the fold until scrolled or
  focused, as in Classic rooms.
- ru.po's 11 dashes are grammatical; for Kate or the translator.
- The update offer is said three times (dialog, toast, status line); which to
  drop is Kate's call.
- Room settings Apply rows stay enabled when nothing changed (behaviour).
- The Rankings header follows the tab; renaming needs a wording decision.
- "Ready with Ryu" from Home's fighter select, the design board's word.
- The tournament organizer screen is empty until the service's first reply.
- A member row with a wide title mark can still drop its status chip at
  1280x720 (older tag behaviour).
- `--check` probe noise from grips, scrolled rows and modals; the shots are
  clean.

## Tests

16/16, run serially: UiPolish, Localization, ControllerNavigation,
MenuButtons, ShellJourney, IdentityJourney, WatchJourney, PublicRoomsJourney,
RankingsJourney, RoomPanelNavigation, V2Layout, MenuSpeech, UiRender,
UiRenderLocales, UiRenderAtlas, UiRenderGallery. A final `--check` sweep at
640x720, 1280x720 and 150% showed only the probe noise. Not run in the game:
the Arcade scroll at 150%, Training's two columns and toast glyphs over a
match.
