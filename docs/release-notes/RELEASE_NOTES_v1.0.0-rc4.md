# SF4 Ember Netplay v1.0.0-rc4 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is an Ember 1.0 test build, published as a GitHub pre-release for testers.** It has everything in v1.0.0-rc3 plus the change below. If you would rather not test, stay on v0.9.9, the current full release. v0.9.9 stays marked Latest, and the updater only follows Latest.

## Changes since v1.0.0-rc3

### Saved replays

- **Replays saved after an online match now play back correctly.** USF4 records one input per frame during a match. When Ember rolled back to correct a late input, the game recorded those frames a second time. From the first rollback, the saved replay fell out of step. On playback, the fighters did things that never happened in the match. Ember now rewinds the game's replay recording along with the rollback.
- **Earlier replays stay broken.** The fix only applies to replays recorded on rc4.

## Compatibility

Install the complete v1.0.0-rc4 package on both machines. Both players need rc4 to play each other.

## What to report

All 80 automated tests pass on the full build, and an independent code review approved the fix. The developer recorded a replay on one PC with a rollback forced every 8 frames, over 1,100 rollbacks in one match. It played back correctly in a normal session. A replay from a real two-PC online match has not yet been checked. We still need these tests:

- **A saved online replay.** Play a real online match with some rollback, save a replay and play it back. Tell us whether it matches what happened.
- **Typing a name.** If typing ever closed your game on an earlier build, try it on rc4 and tell us whether it still happens.
- **A two-PC rematch series.** Play back-to-back rematches, ideally with a spectator, and report any crash or other problem.
- **Changing appearance at a table** while someone else is in the room.
- **Crash logs.** If the game crashes, send the `%APPDATA%\sf4e\logs` folder, including any crash dump.

## Install and play

Extract the complete rc4 package into a new, empty folder on each machine and run `Launcher.exe`. Running `preflight.cmd` first is optional.

See the [player guide](../guides/USER_NETPLAY.md) for play instructions.
