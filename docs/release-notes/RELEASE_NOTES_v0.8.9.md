# SF4 Ember Netplay v0.8.9

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes

- Fixes the mid-match crash "GGPO Assertion Failed: _size != (N-1) @ ring_buffer.h:39". It happened when the other side stopped acknowledging inputs for about a second, such as a spectator that stalled or left, or an opponent whose connection had become one-way. That peer is now disconnected normally instead of crashing the game.

Validated with the local test suite. No other gameplay or rollback behaviour changes.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.8.9` ZIP, run `preflight.cmd`, then `Launcher.exe`. Both players should use the same package.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md). Keep invitations private.
