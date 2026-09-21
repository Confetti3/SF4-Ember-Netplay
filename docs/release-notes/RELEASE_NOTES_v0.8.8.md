# SF4 Ember Netplay v0.8.8

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes

- Ready up and room creation UX improvements. The post-match table no longer flickers, one Ready press carries through to the rematch and any failure is shown as a popup, creating or joining a room goes straight to the lobby, and the match HUD is smaller and quieter by default.
- `preflight.cmd` now works from any install folder.

Validated with the local test suite and two-PC play on the same network. No gameplay or rollback behaviour changes.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.8.8` ZIP, run `preflight.cmd`, then `Launcher.exe`. Both players should use the same package.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md). Keep invitations private.
