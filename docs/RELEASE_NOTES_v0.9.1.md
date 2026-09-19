# SF4 Ember Netplay v0.9.1

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes

- **A rematch no longer fails because a spectator is still finishing the last game.** A spectator that had not yet closed the previous game was left out of the next game's permission but still listed to the fighters, who then refused the rematch. The fighters are now shown exactly the players the game was authorized for, and the spectator rejoins for the game after.
- **A spectator told to connect in the same moment it is admitted now connects.** The instruction used to be dropped if it arrived before the spectator had applied its admission, leaving it waiting for the whole game.
- **Large rooms no longer report a lost connection while a big room update is still arriving.** Room members now answer several requests on one connection at once, so a large update cannot queue the regular health checks behind it and make both sides believe the room had lost its majority.
- **A spectator that stops responding is dropped before it can disturb the fighters.** Spectator backlog is checked five times a second instead of once, and a spectator 48 frames behind is dropped at once. A spectator that is only slow still has a full second to catch up, as before.

Validated with the local test suite, the helper's tests, and the room and recovery network fixtures, including sixteen-member four-table runs on direct and relay-only routes.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.1` ZIP, run `preflight.cmd`, then `Launcher.exe`. Both players should use the same package.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](USER_NETPLAY.md) and [troubleshooting](TROUBLESHOOTING.md). Keep invitations private.
