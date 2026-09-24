# SF4 Ember Netplay — local implementation and validation

Local candidate, 8 September 2026. No release was published, no remote repository was renamed, and no running VPS infrastructure was changed. Legacy code is retired in this source candidate; acceptance of that retirement remains gated on actual SF4 gameplay.

## Scope and preservation

Worktree: `sf4-display-dxvk`, branch `feat/display-dxvk`, original HEAD `b7fcbf1`. The complete pre-implementation dirty tree, including untracked display files and the wrapper binary, was preserved using an alternate Git index in `snapshot/pre-ember-20260908` at `7e3fca9818503e68ae4c5d183112b86a6884f6e9`. The original index was not replaced and the checkout was not reset, cleaned or stashed.

The accepted fixed-shell/Iroh plan is implemented with the user's subsequent amendments:

- Use the exact approved ink-smoke artwork, replacing the triangular mark. Keep the product name **SF4 Ember Netplay**.
- Remove the borderless integration entirely. Native game Options owns display configuration. DxWrapper, its download script, loader, settings, recovery command and package requirements are removed. The fixed ImGui menu still fills the game viewport.
- Preserve internal `sf4e` identifiers, executable names, settings locations, the settings location and executable compatibility. Version 0.8.0 changes the repository to `Confetti3/SF4-Ember-Netplay` and uses the `sf4-ember-netplay-` ZIP prefix; legacy users install into a new folder.

## Implementation

`ApplicationShell` owns a fixed viewport-sized menu with persistent header, navigation, scrollable content and contextual footer. Play, Fighter Gallery, Settings and Help & About replace floating player windows. Room/selection sections stack at narrow widths; wide content is centered and bounded. The shared theme includes status text, focus colors, typography and the approved ink-smoke mark. The existing artwork loader keeps aspect ratios and stable missing/loading placeholders. All current fighter, edition, costume, color, Ultra and stage options remain, including host stage authority and readiness locks.

`OverlayPresentation` controls safe-menu visibility and capture. Loading and fights suppress interactive UI; the passive bottom-center HUD reports unavailable measurements explicitly. F10 toggles at safe menus, controller Start opens, and Escape/Back closes a popup or returns through the shell before hiding Play. Hiding never leaves a room. Post-match reopening waits for a safe native menu. Offline returns input to native menus. Keyboard/mouse capture and native pad suppression share the visibility decision; held pad buttons are suppressed until released. Rollback playback is evaluated before menu capture. This source policy still requires native input acceptance below.

The overlay integration now handles DX9 lifecycle, input, copied snapshots/actions and render entry points. Player rendering lives in the shared UI. Inspectors are extracted into `sf4e__DeveloperOverlay.cxx`, built only with default-off `SF4E_DEVELOPER_UI`, and displayed in the fixed Developer page. SessionController owns session generations and transitions; unused presentation navigation was removed. Stale commands remain rejected.

`ApplicationServices` provides one bounded worker for updates, redacted diagnostics and recovery handoff. Views distinguish helper startup/readiness/failure, member roles/readiness, settings failures and update progress. Diagnostic history is bounded to 16 distinct transitions; export accepts typed connection facts and measurements, excluding names, invitations, capabilities, credentials, arbitrary logs and settings backups. Invitation copying has a brief confirmation. Settings includes profile/netplay, room defaults, current controller guidance and HUD/interface preferences. Graphics and mappings remain in native Options.

The launcher goes directly to SF4 on successful startup. A small Win32/DX9 ImGui recovery surface covers missing game selection, launch/injection errors, missing-helper guidance, retry and updates. Update download progress and cancellation share the application worker. SHA-256 and package inventory validation precede updater handoff. The updater waits for the owning launcher; the game is asked to close through the ordinary window path. No unrelated process is force-killed. Initial injection failure cleanup remains confined to the newly created suspended game and its helper.

Qt, the legacy wizard, Electron IPC shell, broker URI/room codes, Simple/Advanced networking, GNS transports, RelayHost, NAT/UPnP orchestration and GGPO session-tunnel fallback are removed from active source/build/package paths. Shared session authority/protocol and native GGPO UDP remain for Iroh's authenticated loopback bridge. Old operator implementations/scripts are removed locally; historical VPS and migration documents are preserved under `docs/archive`. No server action was taken.

Settings retain one-way imports and backups for names, selections, lobby defaults and valid controllers. Obsolete host/join ports and floating/debug-window flags are removed from active preferences. `PackageInventory.inc` is shared by packaging, preflight and both native updater validators. Upgrades back up only known obsolete product paths; user files, unknown DLLs and recovery backups are preserved. Transaction failures restore earlier replacements and report incomplete restoration if a filesystem error prevents recovery.

## Approved artwork

The approved original is `src/ui/ember.png`, copied byte-for-byte from `exec-ff601ed8-cf4e-4e1b-832b-814bef203757.png`. SHA-256: `ec9f48910ea1a7e8f0337bf60243d47be214d05278497c86ac5d2561e3d7fde8`.

The user supplied [Kyoto National Museum's masterworks of ink painting](https://artsandculture.google.com/story/masterworks-of-ink-painting-kyoto-national-museum/ywXxnuF1XTi-JA?hl=en) as visual direction. This is original generated ink-smoke artwork, not a reproduction of a museum work or an official Capcom mark. The user selected the more expressive smoke image after a simplification was considered; the simpler alternate is not used.

`scripts/build-ember-brand.py` performs size/format conversion only: a multi-size Windows icon and 128x128 RGBA atlas bytes. Normal CMake builds embed those checked-in bytes into the existing ImGui font atlas, so the mark follows the existing DX9 device-reset lifecycle without an additional runtime file dependency. Launcher, Sidecar, Updater and the helper carry the shared icon and branded Windows metadata.

Generation prompt:

> Use case: logo-brand. Asset: final original logo symbol for SF4 Ember Netplay, an unofficial Ultra Street Fighter IV netplay app. The user specifically wants an INK CLOUD OF SMOKE inspired by the energetic sumi-e ink animation in Street Fighter IV cinematics. Design one compact abstract cloud of smoke in expressive calligraphic brushwork. Two or three irregular curling ink plumes rolling sideways and upward, a substantial cohesive smoke silhouette with broken dry-brush edges and a handful of restrained ink flecks. Mostly warm ivory (#F3EBDD) ink with a few restrained ember-orange (#FF8738) brush accents woven into the lower smoke. It will sit on a charcoal UI; negative spaces should be transparent. Genuine transparent PNG background, square canvas, centered mark occupying 85% of canvas, simple enough to read as smoke at 32px yet beautifully ink-textured at 128px. No text or letters, no triangle, no A, no chevrons, no mountains, no flame silhouette, no esports badge, no enclosing circle, no gradients, no glow, no photorealistic volumetric smoke, no background rectangle. Original Japanese brush-painted smoke mark, dynamic and restrained.

## Validation record

Build environment: Visual Studio 18 Build Tools, x86 CMake/Ninja product, Rust 1.98.0 x64 helper. `msvc-build/ember-clean` uses a newly resolved vcpkg installed tree: 10 target dependency packages plus two host CMake helpers, restored from the local binary cache. It does not use the old Qt/GNS installed tree. This is a fresh dependency resolution, not an empty binary-cache rebuild.

The ink candidate passed its x86 product/x64 helper builds (including the developer configuration), all 24 retained CTests, Rust fmt/Clippy/tests (19 unit tests plus both explicitly enabled production-helper network tests), and 1,533 DX9 frames across seven viewport/DPI configurations. Logs and screenshots are local ignored evidence under `build/`. The mouse-cursor follow-up below adds a regression that the original render-only harness did not cover.

| Check | Evidence |
| --- | --- |
| Product and x64 helper, developer UI OFF | `build/ember-native-final-build.log` |
| Product with developer UI ON | `build/ember-ink-developer-build.log` |
| Retained native/transport/regression CTests | `build/ember-ink-ctest.log` |
| Rust fmt, Clippy `-D warnings`, tests including ignored network tests | `build/ember-ink-rust.log` |
| Real DX9 artwork/layout/reset harness | `build/ember-ink-ui.log`, `build/ember-ink-ui/*.bmp` |
| Isolated local installation | `build/ember-ink-install.log`, `msvc-out/ember-ink` |
| Shared inventory, package preflight, dependency notices and ZIP | `build/ember-ink-package.log` |

The DX9 harness covers 720p, 1080p, 1440p, ultrawide, narrow layouts, and 100–200% DPI, including reset/resize, stale geometry, navigation, long errors/names, room/readiness/spectator states, empty/missing artwork and fixed footer/header visibility. Artwork-enabled runs decode 44 native portraits, 245 alternate costumes, 28 stages, 79 Ultra images and 757 numbered color cutouts. These are rendered test fixtures, not actual SF4 match evidence.

Iroh CTests use real local helper processes and normal/forced public relay routes. Rust's explicitly enabled network tests cover production-helper host/join/rematch/UDP bridge behavior on one machine. They do not prove a direct different-network path or an SF4 fight. Installer tests cover missing helper, unexpected files, traversal, legacy removal, preserved user files/backups and rollback after a late filesystem failure. The display-retirement fixture additionally verifies known wrapper removal/backup while retaining a user-owned `d3d9.dll`.

Before the final logo/display change, native SF4 was launched once through the local candidate: Sidecar injection and handshake succeeded, and the fixed Settings page was observed in the actual game. The game window disappeared before the attempted Escape interaction could be confirmed; the cause was not established. This proves startup/rendering of that earlier candidate only.

## Open acceptance gates

The replacement is a local reviewable candidate, not a gameplay-certified release. Legacy retirement is not accepted as complete until these gates have evidence:

- Actual SF4 host/join, fight, repeated rematch, leave/rejoin and offline play with the old VPS unavailable, over direct and forced-relay paths.
- Actual SF4 spectators, including join/leave and repeated match transitions.
- Native mouse/keyboard/controller navigation, clipboard entry, held-button transitions, Alt-Tab, minimize/restore, loading and automatic post-match reopening; verify no native-menu input leakage and no interactive overlay during fights.
- Different-network and clean-machine results, including launcher recovery, missing game/helper, cancelled downloads and updater shutdown/restart handoff.
- Upgrade of a real previous user package through the actual updater. Transactional mock-fixture coverage and package validation do not replace this gate.

The former Safe Display gate is retired at the user's request because the borderless integration was removed. The game retains its native display options. Public discovery, rollback algorithm changes, new renderer work, infrastructure shutdown and publication remain outside this candidate.

## Mouse cursor follow-up

The user reported the pointer flashing/disappearing, rather than the overlay itself flickering. The window-message integration called `ImGui_ImplWin32_WndProcHandler` but discarded its return value. In particular, `WM_SETCURSOR` falls outside the mouse-message range previously consumed by the overlay, so the native handler could replace or hide the cursor immediately after ImGui selected it. Recovery also discarded that result before calling `DefWindowProc`.

`Win32Input` now preserves handled results while the interactive overlay owns input. It disables ImGui's OS cursor changes while the menu is hidden or unfocused, both before Win32 `NewFrame` and during message dispatch. Recovery preserves handled results as well. The native window retains its border/resize cursor handling; the fix does not alter rollback or simulation.

`Win32CursorTest` runs the real ImGui Win32 backend and the shared production message route on a hidden window, with a game-style fallback that clears the cursor on unconsumed messages. The pre-fix implementation lost the cursor after **100/100** movements (`build/cursor-red.log`, exit 1). The fixed implementation lost it after **0/100**, and passed 20 ownership transitions, arrow/text shape changes, native border behavior and retained keyboard routing (`build/cursor-green.log`). This reproduces and fixes the message-fallthrough pattern; it does not substitute for the user's native SF4 retest.

The product rebuild is recorded in `build/cursor-fix-build.log`; focused cursor, visibility and real DX9 UI regressions are in `build/cursor-fix-ctest.log`. A separate installation is available at `msvc-out/ember-cursor-fix`, with installation log `build/cursor-fix-install.log`. The active game is not modified; restarting through that installation is required to load the fixed Sidecar. Native pointer confirmation remains pending until the user retests.

## Combined Ember-room integration

The combined candidate keeps Ember's fixed-shell, Iroh helper, updater and
package inventory while adding the private room authority. A room admits up to
16 members and owns four independent tables. Joining is idle until a member
explicitly queues for a fighter seat or watches a table. Each table has two
fighters and can carry up to 14 spectators.

The same fighter pair keeps its seats until one of them leaves. Both
fighters must ready for each game, and valid native results accumulate wins for
that pair. A seat departure promotes the next queued member and starts a fresh
pair score. Conflicting or missing native result reports remain unresolved;
the host can cancel that game without awarding a win or changing earlier
scores. The host can rename the room, lock admission, change capacity, edit
waiting-table rules and kick members. Host ownership is separate from the P1
seat.

Version 0.8.0 uses the branded `sf4-ember-netplay-` ZIP prefix. The
packaged `START_HERE.md` is copied from `docs/guides/USER_NETPLAY.md`, while the
optional package inventory includes the custom-room and native-result references
under `docs/`. Package assembly therefore retains the Ember runtime set
(`Launcher.exe`, `Sidecar.dll`, `Updater.exe`, `sf4-net.exe`, GGPO and zlib)
without restoring Qt, GNS, RelayHost or the retired broker/VPS services.

The parent integration validation completed the combined C++ build with exit
code 0, 29/29 CTest tests in 161.61 seconds, Rustfmt, 20 Rust unit tests and
Clippy with warnings denied. The room cases included four concurrent tables,
14 spectators on one table, and a forced-relay four-table run. After the room
overview and Ready-footer cleanup, the final integrated UI harness passed
2,016 DX9 frames across seven viewport and DPI configurations with device
resets.

These results cover source, helper, room, and UI regression paths. They do not
establish native SF4 gameplay, two-PC play, clean-machine installation, or a
packaged release acceptance claim.
