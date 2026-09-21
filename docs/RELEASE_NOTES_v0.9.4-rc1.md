# SF4 Ember Netplay v0.9.4-rc1 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It has been measured in a synthetic two-peer harness and passes the local test suite. It has not been played yet. If you want the stable build, use v0.9.3.

## Changes

This build is about matches on poor connections: jitter, short loss bursts and relayed routes.

- **The two players stay in step continuously.** Before, the game only corrected timing when one player was three or more frames ahead, at most once every four seconds, in one lump. A smaller gap was never corrected, so one player took most of the rollbacks. Now the player who is ahead gives back a fraction of a frame at a time, every frame, through waits of at most 3 ms. In the harness the average gap fell from 1.4 to 0.8 frames on a clean link and from 1.1 to 0.5 frames with 40 ms of jitter.
- **A match recovers faster after a burst of lost packets.** When both players are waiting for each other's inputs, the game now resends after 33 ms instead of 200 ms, with backoff and at most four extra packets a second. In the harness this cut the time spent frozen by about 30 percent with 150 ms loss bursts.
- **Connection quality is measured ten times a second instead of once.** This is what keeps the timing correction fresh. It costs about ten small packets a second.

Nothing changes the network format. You can play someone who is still on v0.9.3; you get these changes on your side and they do not.

Both changes are on by default. To turn one off for a comparison, set `SF4E_CONTINUOUS_TIMESYNC=0` or `SF4E_GGPO_INPUT_REPAIR=0` in the shell that starts `Launcher.exe`. If the game feels slow or uneven, try `SF4E_CONTINUOUS_TIMESYNC=0` first and tell us whether it helped.

## What to report

The About screen still shows 0.9.3, so that the updater moves you to the real 0.9.4 when it ships. To confirm you are on this build, look in `sf4e.log` for a line starting `Netplay experiments:` at the start of a match.

After a session, send `sf4e.log` (see [saving logs](SAVING_LOGS.md)) with a note on how the match felt, your input delay, and whether the route was direct or relayed. The lines starting `Pacing [` and `FreezeCandidate` carry the numbers we need.

Known limits: not yet tested in real matches, on relayed routes, or with spectators. Technical detail is in [the experiment notes](experiments/degraded-connection-recovery.md).

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.4-rc1` ZIP into a new folder, run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](USER_NETPLAY.md) and [troubleshooting](TROUBLESHOOTING.md). Keep invitations private.
