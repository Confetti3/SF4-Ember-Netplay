# SF4 Ember Netplay v0.8.3

This update removes two remaining sources of unnecessary recovery-checkpoint work during play and polishes Ember's shared menus, dialogs and controller navigation.

## What's changed

- Keep ordinary room timer ticks checkpoint-free until a result-dispute deadline is actually due. Pending results before the deadline and already-paused disputes no longer rebuild recovery state every tick.
- Retain departed spectators required by a started native match without making those frozen records disable the empty-inbox fast path. They are still pruned only after every relevant authority releases the slot.
- Use one room/runtime snapshot per rendered overlay frame and load selection artwork only while the main Ember application UI is visible. The ImGui frame remains active for F10/Start reopening, training, controller warnings, the compact shortcut and match HUD.
- Expand exported CPU-work diagnostics to nine fixed timing groups and add an explicitly available or unavailable current-room checkpoint-build count. These counters are diagnostic CPU indicators, not displayed-frame measurements.
- Stabilize button colors and availability hints across brief healthy room updates without delaying the live eligibility checks that authorize actions.
- Preserve room-name and chat drafts and keep confirmations attached to their original action through transient updates. Genuine unavailability, removed actions and room or match generation changes still cancel them.
- Correct stale popup rendering, closing-frame activation and same-frame Enter handling. Popups now have independent viewport bounds.
- Reset held and repeated navigation across screen changes, prevent duplicate screen pushes, initialize focus on new screens and prioritize directional navigation over accidental diagonal adjustment.
- Reserve status space so saving and error messages do not move controls, and rebase UI-only deadlines after a Direct3D/ImGui context reset.

## Install or upgrade

For a fresh installation, download **`sf4-ember-netplay-0.8.3.zip`** and its `.sha256` sidecar, extract the complete package into a new writable folder, run `preflight.cmd`, then run `Launcher.exe`.

If you already have the intact published v0.8.2 package, download **`upgrade-sf4-ember-netplay-0.8.2-to-0.8.3.zip`** and its checksum instead. Extract the upgrade into a separate folder, close SF4 and Ember, run **`Install Upgrade.cmd`**, and select the existing v0.8.2 Ember folder. The installer verifies the base installation and payload, builds and preflights the complete v0.8.3 layout locally, backs up replaced files and verifies the result. It does not redownload unchanged artwork.

Everyone in a room must use the same Ember release. Preferences remain under `%APPDATA%\sf4e`.

Requires Windows 10 or later (x64), an owned Steam copy of Ultra Street Fighter IV and the Microsoft Visual C++ x86 runtime.

## Validation and limits

The exact combined Windows source passed the designated 38-test native suite. The UI-polish fixture separately passed one C++14 executable containing 357 checks under Windows/MSVC; its earlier Linux run also passed with AddressSanitizer and UndefinedBehaviorSanitizer. Release packaging verifies the source fingerprint, staged binary hashes, dependency provenance, full-package preflight and native updater inventory. The upgrade builder compares the complete v0.8.2 and v0.8.3 manifests and performs a real temporary upgrade while checking that unrelated user files survive.

The checkpoint/performance candidate was tested successfully by the project owner in SF4 before the UI-polish patch was merged. The later shared-menu behavior has full Windows build and synthetic renderer coverage but has not received the same native gameplay pass. Displayed-frame pacing and input-latency improvements were not measured and are not claimed.

Optional connection checks can still be unavailable even when a match works. The custom room-board toolbar uses a separate rendering path from the shared menu feedback cache and should receive additional visual inspection.

When reporting a problem, save diagnostics and logs from both PCs using the [log collection guide](https://github.com/Confetti3/SF4-Ember-Netplay/blob/release/docs/SAVING_LOGS.md).

Ember remains experimental.

## Attribution

Based on **[sf4e](https://codeberg.org/adanducci/sf4e)** by **Anthony Danducci and contributors**, under the MIT license. This is an unofficial port; Anthony Danducci does not maintain, endorse or support this build. Original licenses and dependency and artwork attribution are preserved.
