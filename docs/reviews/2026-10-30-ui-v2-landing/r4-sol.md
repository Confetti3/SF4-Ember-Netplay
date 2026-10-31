One blocker remains at `82661ac3`: **the UI’s room-settings policy still disagrees with runtime acceptance.** The other round 3 remedies check out.

1. **[P2] Sol 1 residual: visual room settings remain coupled to the controller-layout save gate.**

   **[ApplicationShell.hxx:ShellView::CanEditRoomSettings](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/ui/ApplicationShell.hxx:87)** → **[sf4e__NetplayRuntime__Input.cxx:SaveRoomSettings](/C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__NetplayRuntime__Input.cxx:214)**.

   The new predicate permits room-layout and watched-picture edits whenever this player is not fighting, including while watching a match. All the requested pointer controls now honor that predicate.

   However, the committed runtime does **not** take these saves whenever asked. `SaveRoomSettings` immediately returns unless `CanEditButtonLayoutNow()` succeeds, and that function requires `AtMainMenu()`. The typed `commands::RoomSettings` handler calls this exact path.

   Consequently, while watching a match outside MainMenu, dragging the picture or changing its settings updates the shell’s preferences and submits a request that the runtime silently drops. `UpdatePreferenceSave` then times out and reports a failed save; Retry saving repeats the rejected request.

   `RoomSettingsFollowOneRule` does not catch this: it counts submitted `roomSettings` actions and manually copies their preferences into the view. It proves shell submission, not runtime acceptance.

   **Concrete remedy:** separate the visual room-settings policy from controller-layout reconfiguration. Allow the visual subset during spectating under a policy shared by shell and runtime, while retaining the existing gate for button-layout changes and `Gamepads::SetLayouts`. Add runtime coverage with `AtMainMenu()==false`, a joined room and no local fight, asserting that a visual edit is applied and acknowledged while an unauthorized controller-layout change is refused. Restoring the MainMenu restriction only in the shell would leave the requested watched-picture editing unavailable.

The remaining fixes are resolved in committed source:

| Round 3 item | Round 4 disposition |
|---|---|
| Sol 1: pointer gating | Rows, grips, Hide/Show, More choices and floating-picture movement share the capability. Runtime persistence remains finding 1. |
| Sol 2: Retry saving | `MenuEntry::footer` and `GameMenu::DrawBody` give custom bodies a shared footer; eligibility uses `MaySave`, and shell dispatch precedes panel dispatch. |
| Sol 3 / Opus 2: test decomposition | Dialog tests moved into `menu_dialog_test.cxx`, linked into the existing target. `controller_navigation_test.cxx` is 800 lines. |
| Sol 4: parent rail focus | `ShowTab` replaces the tab/panel stack and sets the parent head’s focus before pushing the panel. The rail-click/Back journey covers it. |
| Opus 3: ownership residuals | Language-save status lives in `SettingsScreens::Status`; tournament focus restoration lives in `IdentityScreens::Rows`. |
| Opus 4: popup hints | `labelOnly` suppresses hit-testing and mutes hints beneath pointer-owning dialogs. |
| Opus 5–6 | Hidden-frame saving is documented and covered; the displaced comments are restored. |
| Opus 1: SettingsStore scope bleed | The UI round-2 commit no longer contains those hunks; HEAD’s test and preference table agree. |

The branch otherwise makes substantial structural improvements: canonical Settings panels replace the legacy-page adapter, obsolete fighter journeys are removed, and shared row geometry, shortcuts and navigation transitions replace duplicated rules. I found no additional high-conviction structural blocker or file crossing from below 1,000 lines to above it.

Validation used existing binaries only:

- **16 selected executables passed.**
- Headless UiRender passed **4,365 frames at 1920×1080/100%**.
- CTest logging was denied; direct ShellJourney and IdentityJourney runs were blocked by temporary-directory permissions. SettingsStore exited with **`0xC0000409`** without diagnostics.
- The provenance receipt names an older revision, so these results do not establish a fresh build of HEAD or gameplay acceptance.
- The scoped diff check reported seven trailing-whitespace lines in `room_panel_navigation_test.cxx`; these are not additional structural blockers.

No files were edited or built. The crash-upload commit and excluded working-tree changes were left outside the review.

VERDICT: CHANGES REQUIRED
