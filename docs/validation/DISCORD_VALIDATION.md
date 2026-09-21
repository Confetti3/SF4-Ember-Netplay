# Discord candidate validation — 2026-09-08

Status: packaged local candidate. **Real Discord invitation and SF4 gameplay acceptance is pending.** The owner confirmed the `ember` artwork upload and reported that a second PC/account is not available now. Do not label this feature accepted or release-ready from these results alone.

## Source and package

- Independent source snapshot: `sf4-discord`, baseline commit `c4c3e6c`. Ongoing controller/UI changes from `sf4-ember-rooms` were included. All 2,336 original snapshot files still matched their recorded SHA-256 hashes after implementation.
- Latest package: `dist/sf4-netplay-launcher-discord-20260908-candidate2.zip`.
- ZIP SHA-256: `05b150abd0ab60671374289c3b317fcc2edc83d2d1d59a8b782be1f9292e8d2a`.
- Application ID `1546980049692135514`; SDK 1.10.19337, pinned archive hash in `cmake/discord-sdk-pin.json`.
- PE machine checks: Launcher and Sidecar are x86 (`014C`); companion, Discord SDK DLL and existing networking helper are x64 (`8664`). The SDK DLL's imported dependencies are Windows system libraries.

## Completed checks

| Check | Result and scope |
| --- | --- |
| Full C++ build | Passed with VS 2026 Build Tools, game x86 and separate SDK companion x64. |
| Rust formatting | `cargo +1.98.0 fmt --all --check` passed. |
| Rust tests | 23 library tests passed, including exact 127-character tickets, malformed/expired tickets, guest re-export and host build rejection. |
| Rust Clippy | `cargo +1.98.0 clippy --locked --offline --all-targets -- -D warnings` passed. |
| CTest suite | 25/25 passed. UI needed desktop graphics access; sandbox alone could not create a DX9 device. |
| Final runtime adjustment | SessionController, DiscordPresence, DiscordBridge, PadAssignment and RuntimeBootstrap rerun: 5/5 passed. |
| UI evidence | 2,862 DX9 frames across nine viewport/DPI configurations with device resets. Pending-invite Switch/Cancel checks passed; wide and narrow screenshots visually inspected. Frames at `build/discord/ui-evidence/`. |
| Networking integration | Normal and forced-relay `IrohRoomIntegrationTest` passed: three room generations and 30 ready/load/rematch cycles per mode. Includes joining with the Discord ticket and equality of host/guest invitation exports. Network access was required. These tests do not run SF4 gameplay. |
| Live Discord RPC | Official companion connected to the running desktop Discord client, registered the adjacent quoted Launcher command, published Starting Ember, cleared activity, and exited cleanly. No invitation was sent and SF4 was not launched. |
| Package validation | Full package passed file inventory, obsolete-file exclusions, manifest hash checks, 1,794 artwork images, and the native updater inventory. Missing SDK runtime and unset application ID were explicitly rejected in negative fixtures. |
| Source whitespace | `git diff --check` passed. |

The transient linker failure while the networking test held its executable was resolved by waiting for the test to exit and rebuilding. No user game or Discord process was terminated. Tests owned only their fixture helpers and companion processes.

## Outstanding acceptance

Follow `docs/guides/DISCORD.md` on two PCs with two Discord accounts and identical candidate2 packages. Host and guest invitations, joining while running, launch from closed, idle admission, table selection, queued/ready/fighting/spectating states, real controller readiness, duplicate/replacement/cancellation, safe switching during offline and online sessions, full/locked/expired rejection, Discord restart, clean game shutdown and ordinary gameplay all require direct observation. Include a different-network join. The successful local RPC and relay checks do not substitute for these results.
