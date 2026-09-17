# SF4 Ember Netplay v0.8.11

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes

- **Ready no longer gets stuck after a quick Unready.** Pressing Ready right after your own Unready (or the other way round) sent the room a table revision it had already moved past, and the refused press sat as "Readying up..." for 20 seconds before blaming the previous match. The room's refusal carries its current state, so the press is now resent from it at once, up to three times; the player sees nothing. Any other refusal of a Ready is reported immediately with the room's reason.

Follows v0.8.10, whose two-PC test showed the room fence closing 27 times in seven matches instead of over a thousand, and no post-match churn. Validated with the local test suite.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.8.11` ZIP, run `preflight.cmd`, then `Launcher.exe`. Both players should use the same package.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](USER_NETPLAY.md) and [troubleshooting](TROUBLESHOOTING.md). Keep invitations private.
