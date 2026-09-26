# SF4 Ember Netplay v0.9.9-rc3 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It has everything in v0.9.9-rc2 plus the changes below. If you would rather not test, stay on v0.9.8, the current full release.

## Changes since v0.9.9-rc2

### Matches

- **Spectators whose game loads a little later than the host's are no longer dropped.** The host used to give a spectator 3 seconds from its own match start, so a spectator could be dropped just as it finished connecting. It now waits 5 seconds counted from the moment the two fighters are synchronized.
- **A dropped spectator returns to the room right away.** Before, it waited a minute and then stayed stuck until the fight ended. The message now says the stream was dropped and to press Watch again for the next match.
- **The "Room control is recovering" notice shows once per episode.** Before, it repeated 60 times a second while a room connection was closing, which hid the message underneath, such as why Ready failed.
- **Random stage.** The stage page has a new first card, Random, and the choice is remembered between sessions. Each time P1 presses Ready or Rematch a stage is rolled, and P2 and spectators get that stage as usual.

## Compatibility

Everyone in the room, including spectators, must use this same package. A room checks that every game runs the same build, so rc3 cannot play rc2, rc1 or v0.9.8.

## What to report

The launcher and updater still report 0.9.8, so that the updater moves you to the real 0.9.9 when it ships.

We especially want:

- **Rooms of 3 or more with someone watching.** Tell us whether spectating starts every time and whether a failed watch returns you to the room within a few seconds.
- **Rooms of 5 or more.** If the room breaks, tell us what the last message on screen said, and send logs from the host as well.
- **Random stage over a few rematches.** Have P1 choose Random and tell us whether the stage changes between rematches and whether everyone sees the same one.
- **Yang, Yun and Rose mirrors online.** Tell us whether the shadows look right and whether the match stays in sync.
- **Matches over an unsteady connection.** Tell us if a fight paused with the countdown and then resumed, or if it ended.
- **Crashes and long sessions**, as in rc2.

After a session, send `sf4e-crash.log` if there is one, `sf4e.log`, the newest `session-*.log` and `launcher.log` (see [saving logs](../guides/SAVING_LOGS.md)), from both players if you can, and say roughly when it happened.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-v0.9.9-rc3` ZIP into a new, empty folder (do not extract over an older version), run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.