# SF4 Ember Netplay v0.9.6-rc2 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It keeps everything from [v0.9.6-rc1](RELEASE_NOTES_v0.9.6-rc1.md), which testers found the best build so far, and fixes problems found in a code review. If you want the stable build, use v0.9.5.

## Changes since rc1

- **Timing correction after a stall.** After the game waits for the other player's inputs, it no longer repays timing correction it had planned before the wait. The numbers from before the stall no longer apply.
- **Returning to the room can no longer hang.** If a match ended but the room never confirmed it, Ready waited forever. Now the match is closed after a minute and you stay in the room.
- **A failed save no longer blocks rematches.** If the match record could not be written to disk, neither player could start the next match. Now the match is released, you see a message, and the save keeps retrying.
- **Rollback refuses game states it cannot save.** A state the rollback system does not support now ends the match with a message, instead of saving a broken state that could crash or desync later.
- **Removed a leftover debug popup** in the sound code that could freeze the game if it was ever reached.
- **The updater can be cancelled while unpacking.** Closing the launcher during an update no longer waits on a stuck unpack, and the updater now uses the system's `tar.exe` by full path.
- **Clearer join error.** Joining with a different version now says both players need the same package.

## Compatibility

Both players must use this same package. A room checks that both games run the same build, so rc2 cannot play rc1 or v0.9.5.

## What to report

The launcher and updater still report 0.9.5, so that the updater moves you to the real 0.9.6 when it ships. `sf4e.log` shows the exact build on the `Netplay experiments:` line at the start of each match.

After a session, send `sf4e.log` (see [saving logs](../guides/SAVING_LOGS.md)) with a note on how the match felt, your input delay, and whether the route was direct or relayed. If both players are on this build, send both logs. We would especially like logs where the first match felt jerky and later ones did not. Please also tell us if the log ever has a line starting `Pacing: limiter runs on thread`.

Known limits: not yet tested with spectators or on Linux. Technical detail is in [the experiment notes](../experiments/degraded-connection-recovery.md).

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.6-rc2` ZIP into a new folder, run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md). Keep invitations private.
