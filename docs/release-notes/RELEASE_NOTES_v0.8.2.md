# SF4 Ember Netplay v0.8.2

This update reduces unnecessary room processing during play, improves the ready-up and post-match interface, and fixes invitations copied through shared clipboards.

## What's changed

- Avoid building and comparing full recovery checkpoints when the room has no work to process. Actual room changes, pending results and recovery operations retain their existing commit checks.
- Fix an overlay focus issue that could prevent F10 or controller Start from reopening Ember after switching between fullscreen and windowed mode.
- Reduce ready-up flicker by keeping battle setup navigation stable, delaying brief "Updating room" notices and holding visible notices through short gaps. Actions still follow the current room state.
- Preserve active-room navigation when Ember reopens after a match instead of returning to the main menu.
- Accept surrounding spaces, tabs and line endings when joining from a copied invitation. Invalid contents, expired invitations and incompatible builds remain rejected.
- Expand exported diagnostics with selected input delay, specific probe authorization rejection reasons, and optional CPU timings for room processing, rollback, save/load and pacing. CPU timings require rollback diagnostics to be enabled and do not measure displayed FPS.

## Install or update

Download **`sf4-ember-netplay-0.8.2.zip`** and its `.sha256` sidecar. Extract the complete package into a new writable folder, run `preflight.cmd`, then `Launcher.exe`. Existing Ember users can also use Help & About to check for updates. **Everyone in a room must use the same Ember release.** Preferences remain under `%APPDATA%\sf4e`.

Requires Windows 10 or later (x64), an owned Steam copy of Ultra Street Fighter IV and the Microsoft Visual C++ x86 runtime.

## Validation and known limitations

The release is built locally from its tagged source. Validation covers the 38-test local native suite, the seven invitation parser tests, a two-peer helper join with a trailing Windows line ending, package preflight and the native updater inventory. Build provenance, source fingerprint, binary hashes and dependency notices are included in the package.

The development candidate was tested in a local two-PC SF4 session. The tester confirmed the display-mode overlay fix, reported substantially reduced ready-up flicker, and successfully joined after the clipboard fix. Play also felt improved, but that subjective report is not a measured FPS or input-latency result. Separate synthetic tests verified idle checkpoint avoidance and three authorized GGPO match generations with four participants; those tests do not establish native gameplay acceptance for every scenario.

- Some ready-up text updates may still be visible.
- Optional connection checks and benchmarks can still return Unavailable even when a match works. One local test succeeded from the guest but failed from the host. Firewall permission and differences between the PCs' clocks are under investigation; this release does not claim to fix that failure. A delay recommendation remains optional, and selected delay is never changed automatically.
- The attempted external frame-time capture produced no usable CSV. Displayed FPS, remaining input-lag reports, different-network play and long-duration stability still need testing.

When reporting a problem, save diagnostics and logs from both PCs using the [log collection guide](https://github.com/Confetti3/SF4-Ember-Netplay/blob/release/docs/SAVING_LOGS.md).

Ember remains experimental.

## Attribution

Based on **[sf4e](https://codeberg.org/adanducci/sf4e)** by **Anthony Danducci and contributors**, under the MIT license. This is an unofficial port; Anthony Danducci does not maintain, endorse or support this build. Original licenses and dependency and artwork attribution are preserved.
