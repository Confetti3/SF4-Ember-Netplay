CHANGES REQUIRED

1. **[P2] Wrapped post-match buttons can become hidden from controller and keyboard users.** [ApplicationShell__PostMatch.cxx:286](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__PostMatch.cxx:286)

   `BarLines` now increases the button bar’s height, but the containing post-match panel remains capped at `420 * Scale()` (`ApplicationShell.cxx:684`). At narrow widths, particularly the four-button Arcade decision prompt, the extra rows can extend below the `Body` viewport. The button loop still calls `PressEntry` with its default `scroll=false`. Moving focus to a clipped button therefore leaves it invisible; this custom navigation does not trigger ImGui’s automatic focus scrolling.

   **Fix:** Account for the measured bar height when sizing the panel, and scroll the focused button into view when focus changes or the bar reflows. Add a test that opens the actual four-button post-match prompt at a narrow viewport and enlarged text, navigates through every button, and checks its rectangle against the child’s visible clip area.

2. **[P2] Entering the new Arcade scrolling layout can strand unchanged focus below the fold.** [RoomPanelTables.cxx:348](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/RoomPanelTables.cxx:348)

   Keep focus on an action such as Ready in the wide, fixed-bar layout, then resize until `follows` becomes true. The newly created `"Arcade floor"` child starts at scroll zero, with the actions below the entire floor. However, `RoomBoardFrame::focusChanged` only compares focus IDs, so the action bar requests no scroll. The cabinet path has the same limitation: `arrivedAt` is populated only when `focusMoved` is true (`RoomPanelArcade.cxx:191`). Focus can remain offscreen indefinitely despite the layout transition.

   **Fix:** Make the scroll viewport own focus visibility, accounting for entry into the scrolling layout and changes to viewport geometry as well as focus changes. Preserve manual scrolling between those events. Test the fixed-to-scrolling transition with unchanged focus on Ready and a bottom-row cabinet. The existing navigation test checks that a probe reported the row, which happens even when its rectangle is clipped; it does not establish visibility.

Reviewed the exact commit and parent through GitHub because the local command runner failed before starting PowerShell. I could not rerun the reported 16 tests. The catalog checks passed for all 56 additions, including placeholders and reuse of existing CJK characters; no changed file crosses 1,000 lines. The Arcade minimum-height formula and XS/S `TagWidth` calculation match their drawing paths, and the Training indent is balanced for its normal multi-entry paths.