# SF4 Ember Netplay v0.9.9-rc4 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It has everything in v0.9.9-rc3 plus the changes below. If you would rather not test, stay on v0.9.8, the current full release.

## Changes since v0.9.9-rc3

### Starting the game

- **An old GGPO.dll next to the game is named instead of a Windows error.** Some players saw "The procedure entry point ggpo_get_last_confirmed_frame could not be located in the dynamic link library Sidecar.dll" on every start, although preflight passed. The game was loading a GGPO.dll left beside SSFIV.exe by another netplay mod or an older install, instead of the one in the Ember folder. The launcher now checks the game folder for GGPO.dll, spdlog.dll, fmt.dll and zlib1.dll before starting the game and shows the full paths of the files to move away. If the game still stops that way, the message says which files to look for, and `launcher.log` records the reason.
- **The game folder chosen in launch recovery is remembered.** Before, a player whose game is not in the default Steam folder had to pick it again on every start. The launcher also reads every Steam library folder, including paths with accented characters, and both formats of Steam's library list.

### Rooms

- **The Random stage card shows the game's own random-select mark.** The card added in rc3 now uses the "?" tile from the game's character select instead of a blank.

### Other

- The launcher no longer bundles the ValveFileVDF library. The notices and attribution files changed to match.

## Compatibility

Everyone in the room, including spectators, must use this same package. A room checks that every game runs the same build, so rc4 cannot play rc3, rc2, rc1 or v0.9.8.

## What to report

The launcher and updater still report 0.9.8, so that the updater moves you to the real 0.9.9 when it ships.

We especially want:

- **Anyone who saw the "entry point ggpo_get_last_confirmed_frame" error.** Tell us whether the launcher now names the file, and which folder it was in.
- **Games installed outside the default Steam folder.** Tell us whether the launcher finds the game on the second start without asking again.
- **Rooms of 3 or more with someone watching.** Tell us whether spectating starts every time and whether a failed watch returns you to the room within a few seconds.
- **Random stage over a few rematches.** Have P1 choose Random and tell us whether the stage changes between rematches and whether everyone sees the same one.
- **Yang, Yun and Rose mirrors online.** Tell us whether the shadows look right and whether the match stays in sync.
- **Matches over an unsteady connection.** Tell us if a fight paused with the countdown and then resumed, or if it ended.
- **Crashes and long sessions**, as in rc3.

After a session, send `sf4e-crash.log` if there is one, `sf4e.log`, the newest `session-*.log` and `launcher.log` (see [saving logs](../guides/SAVING_LOGS.md)), from both players if you can, and say roughly when it happened.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-v0.9.9-rc4` ZIP into a new, empty folder (do not extract over an older version), run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
