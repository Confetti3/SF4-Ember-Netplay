# SF4 Ember Netplay v0.9.9-rc2 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It has everything in v0.9.9-rc1 plus the changes below. If you would rather not test, stay on v0.9.8, the current full release.

## Changes since v0.9.9-rc1

### Matches

- **Shadow moves such as Yang's Super (Seiei Enbu) now roll back correctly.** A rollback left each shadow's animation, hitboxes and timing on the discarded timeline, so the shadows could hit on different frames on each PC and the match desynced. The shadows also lagged or stood still on screen after rollbacks. The fix covers every shadow move, including Yun's Genei Jin and Rose's Soul Illusion. It was confirmed offline with Yang; please test it online.
- **A short connection drop pauses the fight instead of ending it.** The fight now waits up to 8 seconds, showing the "Connection unstable" countdown, and resumes when the connection comes back. Before, a drop of 3 seconds ended the match as a disconnect. A connection that stays down still ends the match after about 8 seconds.

## Compatibility

Everyone in the room, including spectators, must use this same package. A room checks that every game runs the same build, so rc2 cannot play rc1 or v0.9.8.

## What to report

The launcher and updater still report 0.9.8, so that the updater moves you to the real 0.9.9 when it ships.

We especially want:

- **Yang, Yun and Rose mirrors online.** Tell us whether the shadows look right and whether the match stays in sync.
- **Matches over an unsteady connection.** Tell us if a fight paused with the countdown and then resumed, or if it ended.
- **Crashes and long sessions**, as in rc1.

After a session, send `sf4e-crash.log` if there is one, `sf4e.log`, the newest `session-*.log` and `launcher.log` (see [saving logs](../guides/SAVING_LOGS.md)), from both players if you can, and say roughly when it happened.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-v0.9.9-rc2` ZIP into a new, empty folder (do not extract over an older version), run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
