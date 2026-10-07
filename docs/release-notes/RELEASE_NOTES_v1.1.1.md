# SF4 Ember Netplay v1.1.1

v1.1.1 fixes keyboard-only players being sent to Assign Controller, explains two launch failures caused by administrator settings, and adds sets up to first to 10. Fighter and costume choices now follow DLC ownership. Public rooms also use less of the server.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **Everyone in a room needs 1.1.1.** 1.1.0 and 1.1.1 cannot join each other's rooms.
- **From 1.1.0, use the updater.** Ember offers 1.1.1 in game. You can also download the zip from the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.1.1) and extract it over your Ember folder. Your settings carry over.
- **New installs can use setup.exe.** The installer is also available on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.1.1).

## Changes since v1.1.0

- **Keyboard-only players no longer get stuck in Assign Controller.** With no controller connected, the first key press in Ember could open Assign Controller, which a keyboard cannot complete. This is fixed. Reported and diagnosed by FRaccie in [#22](https://github.com/Confetti3/SF4-Ember-Netplay/issues/22).
- **The launcher explains "Could not start the game (Win32 740)".** This happens when SSFIV.exe is set to run as administrator. The launcher now tells you to clear "Run this program as an administrator" in SSFIV.exe's Compatibility settings ([#21](https://github.com/Confetti3/SF4-Ember-Netplay/issues/21)).
- **The launcher explains "Steam must be running" when Steam runs as administrator.** When this causes the game to close, the launcher tells you to restart Steam normally. Thanks to OOPMan for finding this cause ([#18](https://github.com/Confetti3/SF4-Ember-Netplay/issues/18)). The original report in #18 had no administrator settings and stays open.
- **Sets of first to 1 through 10.** Set length now offers every first-to value from 1 through 10, equivalent to best of 1, 3, 5 and so on through 19. Tournament matches offer the same range.
- **DLC choices follow what you own.** Fighter select previously offered DLC costumes based on installed packs. It now offers only the costumes your Steam account owns. Separately sold fighters you do not own keep their roster cards but cannot be picked. A saved pick of one falls back to Ryu. Other players' picks are never checked against what you own.
- **Public rooms use less of the server.** The room host and its networking helper now sleep while a room is idle. The server limit stays at 20 public rooms.

## Known issues

- **Spectators cannot stop watching until the game ends** ([#23](https://github.com/Confetti3/SF4-Ember-Netplay/issues/23)). FRaccie's proposed fix is being looked at.
- **A message sent just as its sender leaves can be missed.** Messages already received stay in Chat.
- **A two-player private room cannot survive its host's game crashing.** The remaining player still has to open a new room. Leaving normally is fine.
- **Browser links need a fallback on Linux and Steam Deck.** Copy the link and paste it into Ember instead of using Open in Ember.

Report problems in [Discord](https://discord.gg/uPNqF5A5uq) with `sf4e.log` and `launcher.log` from `%APPDATA%\sf4e\logs`, the time it happened and what you were doing. For installer or update problems, include `%TEMP%\sf4-netplay-update.log` too.

## Testing

The full build passed all 106 automated tests. The networking helper passed 290 tests, and all server (bridge) tests passed. Code review went through four rounds before approval.

In game on one PC, using only a keyboard, a test build of the same code opened a public room on the 1.1.1 server; the room applied its rules to all four tables and closed cleanly when left.

A match between two PCs has not yet been tested on this exact build.
