# SF4 Ember Netplay v0.9.6

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

This release collects the three v0.9.6 test builds. Testers played long sets on it, including rooms with spectators, and reported smooth matches on Windows, even on slower PCs.

## Changes since v0.9.5

### Smoother matches

- **Timing correction now works.** Since v0.9.4 the game has tried to keep both players in step by waiting a little when it is ahead. The game's own frame limiter cancelled those waits out. The correction now adjusts the frame limiter itself, by up to 3 ms per frame, and both players correct: the one ahead slows down slightly and the one behind speeds up slightly.
- **VSync is always off.** With VSync on, a frame can only end on a screen refresh, which turns small timing corrections into stutter and adds input delay. The game now ignores the VSync setting. Your config file is not changed.
- **Timing correction after a stall.** After waiting for the other player's inputs, the game no longer repays correction it had planned before the wait.

### Rooms and spectators

- **Changing fighter between games works with a spectator.** After a watched match the host no longer holds the match open for up to two minutes. It closes as soon as the spectator has finished, and never waits more than 10 seconds.
- **No false disconnect messages after a match.** "Opponent disconnected", "Connection unstable" and "A spectator disconnected" no longer appear once the match is already over, for example when your opponent leaves the win screen before you.
- **Room migration is fast.** When the host left a room with two or more other players, the room had no host for about 12 seconds. It now hands over within a few seconds.
- **Room buttons no longer flicker.** Ready, Change fighter and the delay controls stay on through short room updates.
- **Taking a seat is retried automatically** if the room changed at the same moment, as Ready already was.
- **Returning to the room can no longer hang.** If the room never confirmed a finished match, the match is closed after a minute and you stay in the room.
- **A failed save no longer blocks rematches.** If the match record cannot be written to disk, you see a message and the save keeps retrying.
- **Clearer join error.** Joining with a different version now says both players need the same package.

### Stability

- **Safer host shutdown after a watched match.** A late input can no longer make the host's game load a save state that was already released.
- **Rollback refuses game states it cannot save.** The match ends with a message instead of saving a broken state that could crash or desync later.
- **Removed a leftover debug popup** in the sound code that could freeze the game.
- **The updater can be cancelled while unpacking**, and it now runs the system's `tar.exe` by full path.

### Logs

Every 15 seconds of a match, `sf4e.log` gets a `Netplay [` line with ping, how far apart the two games are, rollbacks, stalls and timing correction. The `Netplay experiments:` line at the start of each match shows the exact build.

## Compatibility

Everyone in a room must use v0.9.6. A room checks that all games run the same build, so v0.9.6 cannot play v0.9.5 or the v0.9.6 test builds.

Known limits: players on Linux (Proton or Wine) still report jerky matches and rooms that break after one or two games. If you play on Linux, please send the whole `sf4e\logs` folder from your Wine prefix (for Steam: `steamapps/compatdata/45760/pfx/drive_c/users/steamuser/AppData/Roaming/sf4e/logs`) right after it happens, along with your Proton or Wine version. See [saving logs](../guides/SAVING_LOGS.md).

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.6` ZIP, run `preflight.cmd`, then `Launcher.exe`. Players on v0.9.5 can use the in-app updater or the upgrade ZIP. Test build players should use the in-app updater or the full ZIP.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md). Keep invitations private.
