# SF4 Ember Netplay v1.0.0-rc4 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is an Ember 1.0 test build, published as a GitHub pre-release for testers.** It has everything in v1.0.0-rc3 plus the change below. If you would rather not test, stay on v0.9.9, the current full release. v0.9.9 stays marked Latest, and the updater only follows Latest.

## Changes since v1.0.0-rc3

### Replays

- **Replays saved after an online match play back correctly.** The game records one input per frame while the match runs. Each rollback replays a few frames, and the game recorded those frames a second time, so from the first rollback on the replay fell out of step and the fighters did things that never happened. The recording now rewinds with the rollback. Replays saved on earlier builds stay broken; only new recordings are fixed.

## Compatibility

Install the complete v1.0.0-rc4 package on both machines. Both players need rc4 to play each other.

## What to report

All 80 automated tests pass on the full build, and an independent code review approved the fix. The developer recorded a replay on one PC with a rollback forced every 8 frames and checked that it played back correctly. We still need these tests:

- **A replay from a real online match.** Save the replay after a match with some rollback and play it back. Tell us whether it matches what happened.
- **Everything from rc3:** typing a name, a two-PC rematch series (ideally with a spectator), and changing appearance at a table while someone else is in the room.
- **Crash logs.** If the game crashes, send the `%APPDATA%\sf4e\logs` folder, including any crash dump.

## Install and play

Extract the complete rc4 package into a new, empty folder on each machine and run `Launcher.exe`. Running `preflight.cmd` first is optional.

See the [player guide](../guides/USER_NETPLAY.md) for play instructions.
