# SF4 Ember Netplay v0.9.6-rc3 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It keeps everything from [v0.9.6-rc2](RELEASE_NOTES_v0.9.6-rc2.md) and fixes problems found in the first room test with a spectator. If you want the stable build, use v0.9.5.

## Changes since rc2

- **Changing fighter between games works with a spectator.** After a watched match, the host kept the match open for up to two minutes so the spectator could finish watching, and Change fighter stayed locked until then. The host now closes it as soon as the spectator has finished, and never waits more than 10 seconds.
- **No false disconnect messages after a watched match.** "Opponent disconnected" and "A spectator disconnected" no longer appear once the match is already over.
- **Leaving your seat right after a watched match** no longer shows "Returning to the room" as an error.
- **Room migration is fast.** When the host left a room with two or more other players, the room had no host for about 12 seconds and room actions failed. The room now hands over within a few seconds.
- **Room buttons no longer flicker.** Ready, Change fighter and the delay controls briefly switched off each time the room updated. They stay on through these short updates.
- **Taking a seat is retried automatically** if the room changed at the same moment, as Ready already was.
- **Safer host shutdown after a watched match.** A late input could make the host's game load a save state that had already been released. That is now ignored.

## Compatibility

Both players must use this same package. A room checks that both games run the same build, so rc3 cannot play rc2, rc1 or v0.9.5.

## What to report

The launcher and updater still report 0.9.5, so that the updater moves you to the real 0.9.6 when it ships. `sf4e.log` shows the exact build on the `Netplay experiments:` line at the start of each match.

After a session, send `sf4e.log` and the newest `session-*.log` from the same folder (see [saving logs](../guides/SAVING_LOGS.md)), from every player in the room if you can. If a match desyncs or the room closes on you, the lines just before `Room: closing` matter most.

Known limits: not yet tested on Linux. Technical detail is in [the experiment notes](../experiments/degraded-connection-recovery.md).

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.6-rc3` ZIP into a new folder, run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md). Keep invitations private.
