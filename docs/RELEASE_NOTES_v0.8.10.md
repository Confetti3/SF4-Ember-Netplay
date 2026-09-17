# SF4 Ember Netplay v0.8.10

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes

- **Room controls no longer pause during a fight.** The desync checks that fighters exchange every half second were being written into the room's replicated state, so each one built a full room checkpoint on the host, proposed it to every member, and paused room actions on every PC until that checkpoint was imported. In a 16 minute session today that fence closed 1,169 times during play, for up to four seconds at a time, and the import work ran on the game thread. Those checks are now forwarded directly to the other players and spectators at the table and never touch the room checkpoint.
- **The post-match lobby no longer churns.** After a match, the losing PC's "match finished" report kept being resent every half second until the next match started, and each attempt was another room checkpoint. It stops as soon as the room says the game has already ended. This is also what made **Check connection** fail with "Room proposal busy" between matches.
- Fighter names are centred in the room card and the match HUD, so two names of different lengths no longer sit lopsided around VS.
- The 30 second **Benchmark connection** row is gone. The five second **Check connection** remains.

Validated with the local test suite. Two-PC play on this build has not yet been observed; if the room still pauses, keep both players' `%APPDATA%\sf4e\logs` and send **Help & About → Export diagnostics**.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.8.10` ZIP, run `preflight.cmd`, then `Launcher.exe`. Both players should use the same package.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](USER_NETPLAY.md) and [troubleshooting](TROUBLESHOOTING.md). Keep invitations private.
