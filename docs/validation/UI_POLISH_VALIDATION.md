# UI polish: implementation and validation

Base: v0.8.2, commit `b7f775120726a0dc8a2eb02448e5cacd6f07fea2`.
Scope: shared menu rendering, modal ownership, navigation input and UI deadlines.
No release version, game simulation, GGPO, Iroh, or room-authority rules change.

## Changes

- Mark healthy checkpoint waits separately from actual action eligibility. The shared menu renderer debounces transient enabled/disabled colors and explanatory hints for 250 ms in both directions. Real unavailable states and changes of action label/value/type take effect immediately. The renderer never sends its smoothed entry copy to command dispatch.
- Preserve a text draft or confirmation while the same row is temporarily checkpoint-blocked. Acceptance always rechecks the live enabled flag. Removed rows, changed row types, real unavailability, and existing room/match generation fences still cancel the modal. A confirmation stays bound to its original row ID.
- Close renderer popups immediately when the navigation model no longer owns them, without drawing a cleared draft or stale confirmation. Suppress background activation on the closing frame. Give each popup its own viewport bounds and reserve a fixed feedback area.
- Defer Enter acceptance until InputText has consumed the current frame's characters. The navigation model still controls neutral gating and Back/Enter ordering. Clear the edit-focus cache when a menu resumes after being hidden or recreated.
- Reset directional repeat state when gating input. Gate transitions until release, prevent duplicate screen pushes, initialize focus state for new screens, and prioritize vertical movement over adjustment for a diagonal input.
- Reserve status space on settings, join/create, and help screens as well as room screens, so save/error notices do not reposition the controls.
- Rebase shell save/notice deadlines when ImGui time moves backwards after context recreation. Shared navigation uses an accumulated UI clock. No simulation/network clock is changed.
- Do not label a failed opening attempt as a successful room departure.

## Automated validation actually performed

A Linux C++14 headless fixture compiled the production MenuNavigation.hxx and
MenuFeedback.hxx headers. CMake/CTest passed **1/1 executable**, containing
**357 checks**. The same executable passed with AddressSanitizer and
UndefinedBehaviorSanitizer enabled.

Covered: pending draft preservation and blocked acceptance, confirmation target
ownership, removed/changed/unavailable rows, deferred text acceptance, modal
cancel input ordering, held input across transitions, directional repeat reset,
diagonal navigation, scroll preservation, alternating checkpoint/recovery
feedback, immediate hard unavailability, feedback scope changes and clock rebasing.

Reproduce from the repository root (no Windows/game/renderer dependency):

```sh
cmake -S src/tests/ui-polish -B build/ui-polish
cmake --build build/ui-polish
ctest --test-dir build/ui-polish --output-on-failure
```

For a multi-configuration generator, supply `--config Release` to the build and
`-C Release` to CTest. This small standalone project does not replace or alter
the existing native suite. Its count is not 357 separate CTest cases.

After merging the current `release` checkpoint/performance work into the polish
branch, the same standalone fixture passed under Windows/MSVC. The designated
`scripts/build-current.ps1` pipeline then configured and built the combined
32-bit Windows product, staged it, and passed **38/38** local synthetic tests,
including RoomPanelNavigation, ControllerNavigation, UiRender,
OverlayPresentation and OverlayReset.

## Remaining native acceptance

Actual SF4 gameplay acceptance for the UI-polish changes and visual frame-time
measurements were not run. The successful Windows build and synthetic renderer
tests are not proof that every reported flicker or stutter is resolved. The
earlier checkpoint/performance candidate was tested successfully in gameplay,
but that does not automatically validate these later menu-behavior changes.

Confirm the following in actual SF4 gameplay:

1. Alternate ready/unready and healthy checkpoint waits from two PCs. Shared list
   colors, hints and geometry should remain stable while unavailable actions
   remain blocked. Real errors, disconnects and match phase changes must be immediate.
2. Edit room/chat text through checkpoint waits. Keep the draft, accept the final
   typed/pasted characters on Enter, and cancel only the dialog on Back. No empty
   popup flash, accidental re-open or background activation may occur.
3. Open a destructive confirmation, then remove its target or change room/match
   generation. It must not execute against a replacement row. Test training
   confirmations too; their acceptance must remain explicit.
4. Inspect 640x720 and 1280x960 layouts at 1x and 1.5x interface scale. Save/error
   messages must not move buttons. Long popup text must remain reachable.
5. Switch fullscreen/windowed mode with a save debounce, notice or edit active.
   Verify no long-lived stuck deadline, F10/controller reopening, text focus and
   no input leaking to the underlying native menu.
6. Hold a direction or Select across screen changes; release and press again.
   Confirm predictable navigation, no duplicate pages and no unintended value
   adjustment from a diagonal.

## Deliberately unchanged

- Existing fonts, colors, artwork and general layout identity.
- Room actions' enabled flags, server validation and generation fences.
- (Superseded in v0.8.5: the room board now paints through the shared
  `MenuVisualFeedback` cache. Since v0.8.7 its single explanation line also
  exposes elided and multi-line text on hover.)
- Overlay-wide snapshot copying, selection texture uploads and recovery-checkpoint
  performance work identified in the earlier audits. Those need separate profiling.
- No attempt to convert an underlying networking failure into healthy UI state.
