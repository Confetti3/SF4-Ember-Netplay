# SF4 Ember Netplay v0.9.2

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes

- **The interface is now available in Brazilian Portuguese and Latin American Spanish.** The menu follows the Windows display language and can be set by hand under Interface.
- **A card at launch names the game settings that stop rollback from working.** It appears only when the game's own settings differ from the recommended ones, lists just those, and offers "Don't show again". Frame Rate must be FIXED; VSync off and anti-aliasing off lower input latency.
- **Watching a game no longer keeps a spectator waiting two minutes to leave it.** The player hosting the game holds the connection open so spectators can finish receiving it. A spectator was counting the room's other members and holding its own connection open for the same two minutes, for nobody.
- **A spectator whose game stops arriving now returns to the room by itself.** When the host retires first, the spectator's copy of the game stops receiving and it had no way to close on its own. It now waits fifteen seconds and then returns to the room with a notice.
- **Leaving a queue or stopping watching works while the room is still finishing the last game.** Joining a table is held until every player has confirmed the previous result, which is correct, but leaving was held by the same rule. The player the room was waiting for could be exactly the one unable to leave.

Validated with the local test suite and the late-spectator room fixture, which plays three games in a room where a spectator retires from the second one.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.2` ZIP, run `preflight.cmd`, then `Launcher.exe`. Both players should use the same package.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](USER_NETPLAY.md) and [troubleshooting](TROUBLESHOOTING.md). Keep invitations private.
