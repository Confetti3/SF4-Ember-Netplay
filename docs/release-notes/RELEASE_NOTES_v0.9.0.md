# SF4 Ember Netplay v0.9.0

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes

- **Fighter customization is remembered per fighter.** Costume, colour, ultra, edition, personal action, win quote and handicap are kept for each fighter. Switching fighter brings back what that fighter last used instead of carrying the previous fighter's choices over. Your current choices are kept for the fighter they were made on.
- **Big rooms cost far less each frame.** Every room member used to decode every committed room change on the game thread, so one table's activity dropped frames at the others. Received room checkpoints are now decoded on a worker thread, effects are encoded once, and a room chat that has not changed is no longer resent with every update. On a sixteen-member room the host's per-change cost fell from about 78 ms to about 6 ms, and other members' cost fell in step.
- **Spectators can no longer hold up a match.** Once both fighters are ready the fight starts; a spectator whose connection is late or slow sits that game out and rejoins for the next one. A spectator that never synchronizes, or falls too far behind, is dropped without ending the fight.
- The upgrade installer no longer refuses with "Close Ember and its helper processes" because of a helper running from a different folder.
- A Ready press that raced a table change is retried and answered under the original request, so the room screen no longer waits on a reply that never comes.
- With `SF4E_ROLLBACK_DIAGNOSTICS=1` set, `sf4e.log` now names every room and match operation that ran in an over-budget frame.

Validated with the local test suite, the helper's tests, and the room and recovery network fixtures, including a sixteen-member four-table run.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.0` ZIP, run `preflight.cmd`, then `Launcher.exe`. Both players should use the same package.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md). Keep invitations private.
