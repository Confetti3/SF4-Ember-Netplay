# SF4 Ember Netplay v0.9.8-rc2 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It fixes the two problems players reported on v0.9.7 and a set of room, rollback and updater issues found in a code review. If you want the stable build, use v0.9.7.

## Changes since v0.9.7

### Fixes for the reported problems

- **Players with the same name can now join each other.** Every install was named "Player" until you changed it, and a room turns away a second player with a name it already has. New and unchanged installs now get a name like "Player 4821". If your name is still exactly "Player", it changes the next time you start the game. You can rename yourself in Settings as before.
- **A refused join now says why.** A join the host turned down used to show "Could not join the room. Check the invitation and that both players use the same package", followed by "Room control is recovering". You now see the actual reason, for example "That player name is already in the room. Change it in Settings." or "This room is full."
- **The launcher checks your Visual C++ runtime before it starts the game.** A runtime older than the one Ember was built with made the game crash at every start. The launcher now says so and offers to open the download page. Install the x86 version, then start Ember again.

### Rooms

- **Players whose game crashed no longer stay in the room.** If a player's game closed abruptly and the host then left, the room could lose its host and keep a player who was no longer there. The room now removes that player and passes hosting to someone who is still connected.
- **A finished match no longer blocks the next one at its table.** If your profile could not be saved after a match, the table could stay held and the next match there never started.
- **Leaving a room always finishes.** If the room did not confirm that you left, the game kept trying forever. It now leaves on its own after 8 seconds.
- **Room buttons pressed during a brief reconnect are kept.** They go through once the room catches up, instead of doing nothing. Settings the room cannot apply yet now tell you so.

### Matches

- **Rollback sound fix.** Two identical sounds playing at once could cut each other off after a rollback.
- **Less memory churn in long sessions.** Sound state is no longer reallocated on every rollback save.
- **Failed rollback saves are handled.** If the game could not save its state, the match is ended cleanly instead of continuing on incomplete data.
- **The ping in the match display no longer shows a stale value** while the connection is interrupted.

### Updates

- **An interrupted update is repaired at the next start.** If the game or PC stopped during an update, the launcher now restores your install before playing instead of starting on half-replaced files.
- **Update files no longer pile up.** Leftover downloads and old backups are cleaned up, and only the last good backup is kept.

## Compatibility

Both players must use this same package. A room checks that both games run the same build, so rc2 cannot play v0.9.7.

## What to report

The launcher and updater still report 0.9.7, so that the updater moves you to the real 0.9.8 when it ships.

After a session, send `sf4e.log` and the newest `session-*.log` from the same folder (see [saving logs](../guides/SAVING_LOGS.md)), from every player in the room if you can. If the room closes on you, the lines just before `Room: closing` matter most. Linux (Proton) players: this build logs much more when a room closes about 30 seconds after a match, so those logs are especially useful.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-v0.9.8-rc2` ZIP into a new, empty folder (do not extract over an older version), run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
