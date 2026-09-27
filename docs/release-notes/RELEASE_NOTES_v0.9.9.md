# SF4 Ember Netplay v0.9.9

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes since v0.9.8

### Matches

- **Shadow moves such as Yang's Super (Seiei Enbu) now roll back correctly.** Shadows now restore their animation, hitboxes and timing, fixing mismatched hits and shadows that lagged or stood still after a rollback. This also covers Yun's Genei Jin and Rose's Soul Illusion.
- **A short connection drop pauses the fight instead of ending it.** The fight waits up to 8 seconds with a "Connection unstable" countdown and resumes if the connection returns. A connection that stays down still ends the match after about 8 seconds.
- **You no longer end up controlling both characters after a disconnect.** When the match loses its connection for good, the fight ends and you return to the room.
- **A failed match start no longer leaves its network port open.** The next match can prepare cleanly.
- **Your profile record is saved once per match.** It was being saved two or three times.

### Rooms and spectators

- **Match errors no longer close the whole room.** Two kinds of helper errors that concern only one match now leave the room open, and a burst of room events no longer stops the helper.
- **Spectators whose game loads a little later get more time to connect.** The host now waits 5 seconds from the moment the two fighters are synchronized, instead of 3 seconds from its own match start.
- **A dropped spectator returns to the room right away.** You no longer wait a minute and then stay stuck until the fight ends. The message says the stream was dropped and to press Watch again for the next match.
- **The "Room control is recovering" notice shows once per episode.** Repeated notices no longer hide the message underneath, such as why Ready failed.

### Leaving and rejoining

- **You can leave and rejoin the same room through the same Discord invitation without restarting Ember.** The host learns when you leave without waiting for a timeout, and your previous visit no longer blocks your return.
- **Leaving no longer keeps you stuck waiting indefinitely.** If the room does not confirm your departure within a limited time, Ember releases it locally so you can host or join again. You see: "The room did not confirm your departure. You have left locally."
- **Rooms stay responsive after someone leaves.** Finished departures no longer flood everyone with repeated room updates that could block joins and rejoins. A message sent just after someone leaves no longer stops the room from responding to actions.
- **Players clear the former leader's old connection after another player takes over.** Players who did not become the new leader could previously keep that stale connection around.

### Starting the game and the launcher

- **The launcher names conflicting DLLs beside the game before it starts.** It checks for `GGPO.dll`, `spdlog.dll`, `fmt.dll` and `zlib1.dll` beside `SSFIV.exe` and shows the full paths of files to move away, instead of leaving you with a Windows entry point error. If startup still fails that way, the message explains which files to look for and `launcher.log` records the reason.
- **The launcher finds games in other Steam libraries and remembers your chosen game folder.** It reads both formats of Steam's library list, including paths with accented characters. A folder you choose in launch recovery is kept for the next start.

### Stages

- **You can choose Random on the stage page.** The first card uses the game's own "?" mark, and your choice is remembered between sessions. Each time P1 presses Ready or Rematch, a stage is rolled and shared with P2 and spectators.

### Crash reports and diagnostics

- **When the game crashes, Ember writes a crash record.** `sf4e-crash.log` and `sf4e-crash.dmp` are saved in the same logs folder as `sf4e.log`. The crash log names where the game crashed and includes the last log lines.
- **Logs include more detail about matches and game exits.** `launcher.log` records the game's exit code, while `sf4e.log` records memory use at the start and end of each match and the input delay each side applied at match start.
- **Failed room messages have a reason in `sf4e.log`.** The `reason=` detail helps explain why a message could not be delivered.

### Other

- **Networking failure messages stay on screen in every language until networking returns.** In Spanish and Portuguese, messages that networking was unavailable or could not start disappeared after 30 seconds.
- **When the previous match fails to close, you see that message first.** It takes priority over "Your Ready did not go through," which did not explain the cause.
- **The launcher no longer bundles the ValveFileVDF library.** The notices and attribution files have been updated to match.

## Testing

Players tested v0.9.9-rc1 through rc5, and testers report rc5 is working well. The build's automated tests (63) and the networking helper's tests pass. Leaving and rejoining through a Discord invitation and a cable-pull style connection drop are still not confirmed across two PCs.

## Compatibility

Everyone in a room, including spectators, must use v0.9.9. A room checks that all games run the same build, so v0.9.9 cannot play v0.9.8 or any rc test build.

Known limits under Linux (Proton or Wine): `preflight.cmd` cannot check the files, because Wine's PowerShell does nothing, and the in-app updater cannot extract a package. Update by extracting the new ZIP into a new folder.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-0.9.9` ZIP, run `preflight.cmd`, then `Launcher.exe`. Players on v0.9.8 can use the in-app updater or the upgrade ZIP. Players on an rc test build can also use the in-app updater, because those builds report themselves as 0.9.8, or extract the full ZIP into a new folder. If Ember does not open after the in-app update, start it again.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
