# SF4 Ember Netplay v0.9.3

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes

This release is about spectators who were thrown out of the game or the room instead of being returned to the room.

- **A spectator who falls behind no longer has the game close on them.** The player hosting the game stops sending frames to a spectator that is too far behind. The next packet that did arrive skipped frames, and the netcode library treated that as a fatal error and exited the game. It is now an ordinary disconnect: the spectator sees a connection notice and returns to the room.
- **A spectator stays in the room when its connection helper is slow to close the game.** If the helper did not confirm the close within thirty seconds, the spectator was removed from the room with "Match teardown timed out". Only fighters need that protection. A spectator now stops waiting and stays a member.
- **A spectator stays in the room when its game fails during a brief room reconnect.** The failure was read as a lost match and the room was closed. A spectator has no seat to lose, so it now returns to the room like any other ended game.
- **Every room exit is written to `sf4e.log` with its cause.** If you are still thrown out of a room, send that file; it now names the reason.

Validated with the local test suite, a new test that feeds a spectator a skipped-frame packet, a new test for the slow helper close, and the late-spectator and stalled-spectator room fixtures.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.3` ZIP, run `preflight.cmd`, then `Launcher.exe`. Both players should use the same package.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](USER_NETPLAY.md) and [troubleshooting](TROUBLESHOOTING.md). Keep invitations private.
