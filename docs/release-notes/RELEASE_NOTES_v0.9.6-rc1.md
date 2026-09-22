# SF4 Ember Netplay v0.9.6-rc1 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It passes the local test suite, the two-peer timing harness and an offline check in the game. It has not been played online yet. If you want the stable build, use v0.9.5.

## Changes

- **Timing correction now actually works.** Since v0.9.4 the game has tried to keep both players in step by waiting a little when it is ahead. Those waits never reached the frame rate: the game's own frame limiter simply waited less to make up for them. The correction now adjusts the frame limiter itself, by up to 3 ms per frame. In an offline check a 2 ms request turned 16.667 ms frames into 18.667 ms frames, exactly as asked.
- **Both players correct.** The player who is ahead slows down slightly and the player who is behind speeds up slightly, so each closes half the gap. This also means you get the correction when your opponent is still on v0.9.5.
- **VSync is always off.** With VSync on, a frame can only end on a screen refresh, which turns small timing corrections into stutter and adds input delay. The game now ignores the VSync setting. Your config file is not changed, and the launch card no longer asks you to turn VSync off.
- **Better logs for feedback.** Every 15 seconds of a match, `sf4e.log` gets a `Netplay [` line with ping, how far apart the two games are, rollbacks, stalls and how much timing correction was applied.

Nothing changes the network format. You can play someone on v0.9.5.

To compare against the old behavior, set `SF4E_CONTINUOUS_TIMESYNC=0` in the shell that starts `Launcher.exe`.

## What to report

The About screen still shows 0.9.5, so that the updater moves you to the real 0.9.6 when it ships. To confirm you are on this build, look in `sf4e.log` for lines starting `Netplay [` during a match.

After a session, send `sf4e.log` (see [saving logs](../guides/SAVING_LOGS.md)) with a note on how the match felt, your input delay, your opponent's version, and whether the route was direct or relayed. If both players are on this build, send both logs. The lines starting `Netplay [` and `Pacing [` carry the numbers we need. We are especially interested in the `timesyncEvents` count, which we expect to stay near zero now.

Known limits: not yet tested in real matches, on relayed routes, or with spectators. Technical detail is in [the experiment notes](../experiments/degraded-connection-recovery.md).

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.6-rc1` ZIP into a new folder, run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md). Keep invitations private.
