# SF4 Ember Netplay v0.9.7

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes since v0.9.6

### Rooms

- **Set scores are shown.** Rooms already counted wins for the two seated players. The running count, such as 2 - 1, now replaces "VS" on the room's table card and "vs" on the match HUD, for players and spectators. A game counts once both players report the same result, so during a game the HUD shows the score before it. The count starts again at 0 - 0 when the pair changes.

### Community

- **Join the Ember Discord.** Help & About has a "Join the Ember Discord" entry. After you confirm, it opens [discord.gg/uPNqF5A5uq](https://discord.gg/uPNqF5A5uq) in your browser, which may open behind the game in fullscreen.

### Match HUD

- **The ping label reads "Ping" in Spanish and Portuguese.** It was "Retardo" and "Latência", which players confused with the input delay shown next to it.

### Logs

- The `Netplay [` line in `sf4e.log` no longer reports a ping of 0 before the first round trip completes.

## Compatibility

Everyone in a room must use v0.9.7. A room checks that all games run the same build, so v0.9.7 cannot play v0.9.6.

Known limits: players on Linux (Proton or Wine) still report jerky matches and rooms that break after one or two games. If you play on Linux, please send the whole `sf4e\logs` folder from your Wine prefix (for Steam: `steamapps/compatdata/45760/pfx/drive_c/users/steamuser/AppData/Roaming/sf4e/logs`) right after it happens, along with your Proton or Wine version. See [saving logs](../guides/SAVING_LOGS.md).

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.7` ZIP, run `preflight.cmd`, then `Launcher.exe`. Players on v0.9.6 can use the in-app updater or the upgrade ZIP.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
