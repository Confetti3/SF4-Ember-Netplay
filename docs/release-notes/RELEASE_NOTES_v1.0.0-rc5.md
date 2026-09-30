# SF4 Ember Netplay v1.0.0-rc5 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is an Ember 1.0 test build, published as a GitHub pre-release for testers.** It has everything in v1.0.0-rc4 plus the changes below. If you would rather not test, stay on v0.9.9, the current full release. v0.9.9 stays marked Latest, and the updater only follows Latest.

## Changes since v1.0.0-rc4

### Crashes

- **Fixed a crash in long sessions and rematch series.** The game could close with code 0xC0000374 (heap corruption), sometimes after many rematches, sometimes 45 minutes in. The match HUD and the game updated the same connection message at the same moment, and one could free it while the other still wrote to it. The HUD now draws from a copy the game hands it each frame. A test that makes both sides hit that message as fast as they can crashed rc4 within a fraction of a second and runs cleanly on rc5.
- **Fixed rollback restoring the game's sprite animations incompletely.** After a rollback, the game could hand the same animation slot to two owners. Rollback now restores the pool those slots come from as well.
- **A rollback that cannot restore the HUD exactly now ends the match cleanly** with a message, instead of putting back a HUD the fight was never in.
- **Very long rounds no longer write past the replay recording.** With long round times, the game's own replay recorder could overrun its buffer near the end of a round. Recording now stops when a round's recording is full.

### Rooms

- **Fighter options and Stage are back on the table page.** Since rc2, personal action, win quote, handicap and edition could not be changed in a room, and neither could P1's stage. Select on either row opens its page, and a pick or Back returns to the table. Only P1 can change the stage, since theirs is the one the match uses.
- **The table page reads in order.** Under Ready come your own pick (fighter, Ultra, appearance, fighter options), then the match (stage and the table's rules), then your connection (input delay, connection check), then Leave seat.

### Crash reports

- A heap corruption crash now writes a larger `sf4e-crash-*-heap.dmp` of several hundred MB, which shows the damaged memory. The launcher keeps the newest two of those. Like every crash dump, it can hold your room invitation, player names and chat, so send it privately.
- `launcher.log` and `sf4e.log` now start with the build they came from.

## Compatibility

Install the complete v1.0.0-rc5 package on both machines. Both players need rc5 to play each other.

## What to report

All automated tests pass on the full build, and an independent code review approved the changes. We still need these tests:

- **A two-PC rematch series.** Play back-to-back rematches, ideally with a spectator and on a connection that drops now and then, and report any crash.
- **The table page's new rows.** Change personal action, win quote and handicap from a seat, and the stage as P1, and check the match uses them.
- **A saved online replay.** Save one after a real online match and check it plays back as it happened.
- **Crash logs.** If the game crashes, send the `%APPDATA%\sf4e\logs` folder, including any crash dump.

## Install and play

Extract the complete rc5 package into a new, empty folder on each machine and run `Launcher.exe`. Running `preflight.cmd` first is optional.

See the [player guide](../guides/USER_NETPLAY.md) for play instructions.
