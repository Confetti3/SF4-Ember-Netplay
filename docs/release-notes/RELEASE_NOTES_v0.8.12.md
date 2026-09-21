# SF4 Ember Netplay v0.8.12

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes

- **Fixes the round-start crash "GGPO Assertion Failed: FALSE @ sync.cpp:238".** If the opponent's connection dropped before any of their inputs had arrived in that match (the timer still on 99, "Waiting for opponent"), GGPO tried to roll back to a frame that was never saved and killed the game. That drop now ends the match normally with "Opponent disconnected".
- Each match start and every desync report now record the PC's floating-point control state in `sf4e.log`, so a desync between two PCs can be compared. If you see "the two games diverged", keep both players' `%APPDATA%\sf4e\logs` and, if you can, run with `SF4E_ROLLBACK_DIAGNOSTICS=1` set on both PCs for the next session.

Validated with the local test suite, including a new GGPO test for the disconnect case.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.8.12` ZIP, run `preflight.cmd`, then `Launcher.exe`. Both players should use the same package.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md). Keep invitations private.
