CHANGES REQUIRED

1. **[P2] Post-match focus can still become hidden when the layout changes without changing the bar’s line count.** [ApplicationShell__PostMatch.cxx:292](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell__PostMatch.cxx:292)

   `postMatchBarShown_` uses only the focus ID and `lines` as a proxy for layout. However, the buttons’ vertical positions also depend on `words`, the maximum wrapped explanation height calculated above.

   Keep focus on `back` in a narrow four-button prompt after it scrolls into view, then narrow the viewport enough to wrap the explanation onto another line while the button bar remains two lines. `words` increases and moves the focused button downward, but `shown` stays unchanged. `PressEntry` receives `reveal=false`, leaving the button clipped until focus moves again. A viewport-height reduction can produce the same problem.

   **Fix:** Replace the serialized focus/line-count cache with a typed visibility state that tracks the focused button’s rectangle in unscrolled content coordinates and the viewport dimensions. Reveal on focus or geometry changes, preserving manual scrolling between those events. Extend `PostMatchFocusedButtonInView` to resize with unchanged focus and unchanged bar line count, then assert visibility against the clip rectangle. The current test covers navigation at one fixed size and misses this case.

The Arcade fix resolves the reported fixed-to-scrolling transition: both the action bar and cabinets now receive the geometry-triggered reveal request. `RoomPanelNavigationTest` passed locally. The new post-match regression also completed before `ShellJourneyTest` later stopped on a replay fixture’s temporary-directory creation, denied by this read-only environment. The existing post-fix CTest log records **16/16 passing**; I did not independently complete that entire suite.

All 56 locale additions have the expected placeholders, and the CJK additions reuse existing catalog characters. `ArcadeBoardHeight` and XS/S `TagWidth` match their drawing paths. Training’s reachable multi-entry paths retain balanced indentation across the early `continue` branches. No file crosses 1,000 lines, and the scoped whitespace check passes.

