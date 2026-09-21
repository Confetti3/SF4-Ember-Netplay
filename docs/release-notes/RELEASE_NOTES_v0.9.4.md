# SF4 Ember Netplay v0.9.4

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes

This release is about matches on poor connections and about players who close the game without leaving the room.

- **Matches hold up better on poor connections.** The two players now stay in step every frame, instead of correcting only when one gets three frames ahead. After a burst of lost packets the game resends within 33 ms instead of 200 ms. Testers playing across countries went from delay 5 to delay 3 without teleports. These changes were in the v0.9.4-rc1 test build.
- **A player who closes the game (for example with Alt+F4) is removed from the room.** Before, they stayed in the room as a ghost, still holding their seat, which could stop matches from starting for everyone else. After 15 seconds without reconnecting they now leave the room like any other player, and their seat is freed. In a room of only two players the host still has to create a new room.
- **A spectator is no longer locked out of later matches.** If closing a match's connection timed out, the spectator stayed in the room but could not watch again until the launcher was restarted. That connection is now released.
- **The connection timing starts from zero.** One timing value in the netcode was not set until the first measurement arrived, so the first moments of a match could start from a random number.
- **Fewer warnings in `sf4e.log`.** The game no longer resends a "match finished" report after the table has already ended, and a rejected late report is logged as information rather than a warning.

The internal code was also reorganized into smaller files. That work does not change how the game behaves.

To turn off one of the connection changes for a comparison, set `SF4E_CONTINUOUS_TIMESYNC=0` or `SF4E_GGPO_INPUT_REPAIR=0` in the shell that starts `Launcher.exe`.

Validated with the local test suite, the room recovery fixtures including a new one for a player whose game is closed, and test sessions by players.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.4` ZIP, run `preflight.cmd`, then `Launcher.exe`. Both players should use the same package. If you installed the v0.9.4-rc1 test build, install this full package over it.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md). Keep invitations private.
