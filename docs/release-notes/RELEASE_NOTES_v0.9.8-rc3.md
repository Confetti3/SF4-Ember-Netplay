# SF4 Ember Netplay v0.9.8-rc3 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It has everything in rc2 plus the fixes below. If you would rather not test, stay on v0.9.7, the current full release.

## Changes since v0.9.8-rc2

### Rooms

- **Chat can no longer cancel a room action.** If a room button (Queue, Watch, Unready and the like) had to wait for the room to catch up, sending a chat message in the meantime could drop it. Chat now waits on its own.
- **The newest room button wins.** If you pressed Queue while the room was catching up and then Unqueue, the older Queue could still go through afterwards and put you back in line. Only your latest choice is kept now.
- **Battle slots are easier to read.** The fighter portraits sit at the outer edges of each slot and are larger, with the score alone in the middle.

### Updates

- **A finished update is never reported as rolled back.** If cleaning up old backups failed after an update, the updater could say it had restored your previous files even though the new version was installed.

### For the Linux room problem

- **More logging when a match ends and the room closes about 30 seconds later.** The game now logs if its networking helper gets stuck, what it was doing and for how long, and the last message it got from the helper. This is aimed at the problem Linux (Proton) players see after a match. It does not fix it yet.

## Compatibility

Everyone in the room, including spectators, must use this same package. A room checks that every game runs the same build, so rc3 cannot play rc2 or v0.9.7.

## What to report

The launcher and updater still report 0.9.7, so that the updater moves you to the real 0.9.8 when it ships.

After a session, send `sf4e.log` and the newest `session-*.log` from the same folder (see [saving logs](../guides/SAVING_LOGS.md)), from every player in the room if you can. If the room closes on you, the lines just before `Room: closing` matter most. Linux (Proton) players: lines that start with `Helper:` or `Match teardown:` are the ones we need.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-v0.9.8-rc3` ZIP into a new, empty folder (do not extract over an older version), run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
