# SF4 Ember Netplay v1.0.0-rc5 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is an Ember 1.0 test build, published as a GitHub pre-release for testers.** It has everything in v1.0.0-rc4 plus the changes below. If you would rather not test, stay on v0.9.9, the current full release. v0.9.9 stays marked Latest, and the updater only follows Latest.

## Changes since v1.0.0-rc4

### Crashes

- **Fixed a cause of crashes in long sessions and rematch series.** The game and match HUD could update the same connection message at once, causing heap corruption and exit code `0xC0000374`. The HUD now uses a copy supplied each frame. A test that repeatedly updates the message from two threads crashed rc4 within a fraction of a second and runs cleanly on rc5.
- **Rollback now restores sprite animation slots fully.** It also restores the pool those slots come from, preventing the game from assigning one slot to two owners.
- **If rollback cannot restore the HUD exactly, the match ends cleanly with a message.** It no longer restores a HUD state that did not belong to that frame.
- **Very long rounds no longer overrun the replay recording.** Recording stops when the round's recording buffer is full.

### Rooms

- **Fighter options and Stage are back on the table page.** Since rc2, players could not change personal action, win quote, handicap, edition or P1's stage from a seat. Select either row to open its page; making a pick or choosing Back returns to the table. Only P1 can change the stage, since the match uses P1's choice.
- **The table page follows a clearer order.** Ready comes first, followed by your fighter, Ultra, appearance and fighter options; stage and table rules; input delay and connection check; then Leave seat.

### Crash reports

- Heap corruption crashes now write a larger `sf4e-crash-*-heap.dmp`, usually several hundred MB, to help identify the damaged memory. The launcher keeps the newest two. Crash dumps can contain room invitations, player names and chat, so send them privately.
- `launcher.log` and `sf4e.log` now identify their build at the start.

## Compatibility

Install the complete v1.0.0-rc5 package on both machines. Both players need rc5 to play each other. A different build cannot join.

## What to report

All 83 automated tests pass on the full build, and an independent code review approved the changes. The two-thread connection-message test reproduces the rc4 crash and runs cleanly on rc5. Real two-PC rematch series on rc5 have not yet been tested. We still need these tests:

- **A two-PC rematch series.** Play back-to-back rematches, ideally with a spectator and a connection that occasionally drops, and report any crash.
- **The restored table rows.** Change personal action, win quote, handicap and edition from a seat, and the stage as P1. Check that the match uses your choices.
- **A saved online replay.** Save one after a real online match and check that playback matches what happened.
- **Crash logs.** If the game crashes, send the `%APPDATA%\sf4e\logs` folder, including any crash dump, privately.

## Install and play

Extract the complete rc5 package into a new, empty folder on each machine and run `Launcher.exe`. Running `preflight.cmd` first is optional.

See the [player guide](../guides/USER_NETPLAY.md) for play instructions.
