# SF4 Ember Netplay v0.9.9-rc5 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It has everything in v0.9.9-rc4 plus the changes below. If you would rather not test, stay on v0.9.8, the current full release.

## Changes since v0.9.9-rc4

### Leaving and rejoining

- **You can leave and rejoin the same room through the same Discord invitation without restarting Ember.** The host now learns when you leave without waiting for a timeout. Returning players are no longer confused with their previous visit, and old room state from before they left no longer blocks their return.
- **Leaving no longer keeps you stuck waiting indefinitely.** If the room does not confirm your departure within a limited time, Ember releases it locally so you can host or join again. The message is: "The room did not confirm your departure. You have left locally."

### Rooms

- **Rooms no longer get bogged down after someone leaves.** A finished departure was being processed repeatedly, flooding everyone with room updates. This could prevent a new join or rejoin from completing.
- **Players now clear the former leader's old connection properly after another player takes over.** Previously, players who did not become the new leader could keep that stale connection around.
- **A message sent just after someone leaves no longer stops the room from responding to actions.** The host now handles that departure without interrupting the remaining players.

### Diagnostics

- **Failed room messages now have a reason in `sf4e.log`.** The `reason=` detail helps explain why a message could not be delivered.

## Compatibility

Everyone in the room, including spectators, must use this same package. A room checks that every game runs the same build, so rc5 cannot play rc4, rc3, rc2, rc1 or v0.9.8.

## What to report

The launcher and updater still report 0.9.8, so that the updater moves you to the real 0.9.9 when it ships.

We especially want:

- **Leaving and rejoining the same room through the same Discord invitation.** Try it several times without restarting Ember, including right after a match. Tell us whether you can rejoin and queue, and whether you see the local departure message.
- **Rooms of 3 or more after someone leaves.** Tell us whether the room stays responsive and whether new players and returning players can still join.
- **Anyone whose game closes by itself with the "game exited with an error" screen.** Send `launcher.log`, since it records why the game closed.
- **Rooms of 3 or more with someone watching.** Tell us whether spectating starts every time and whether a failed watch returns you to the room within a few seconds.
- **Yang, Yun and Rose mirrors online.** Tell us whether the shadows look right and whether the match stays in sync.
- **Matches over an unsteady connection.** Tell us if a fight paused with the countdown and then resumed, or if it ended.
- **Crashes and long sessions.** Report crashes, freezes or problems that appear after playing for a while.
- **Random stage over a few rematches.** Have P1 choose Random and tell us whether the stage changes between rematches and whether everyone sees the same one.
- **The rc4 startup fixes.** If you saw the "entry point ggpo_get_last_confirmed_frame" error, tell us whether the launcher names the file and its folder. If your game is outside the default Steam folder, tell us whether the launcher finds it on the second start without asking again.

After a session, send `sf4e-crash.log` if there is one, `sf4e.log`, the newest `session-*.log` and `launcher.log` (see [saving logs](../guides/SAVING_LOGS.md)), from both players if you can, and say roughly when it happened.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-v0.9.9-rc5` ZIP into a new, empty folder (do not extract over an older version), run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
