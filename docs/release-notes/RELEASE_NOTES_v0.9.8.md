# SF4 Ember Netplay v0.9.8

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes since v0.9.7

### Linux (Proton and Wine)

- **The room no longer closes about 30 seconds after a match.** At match end the networking helper could stop answering under Wine, and the room then closed with `match_teardown_timeout`. Rematches now work and the room stays open.
- **The launcher starts under Wine.** Its Visual C++ runtime check accepts Wine's own runtime.

### Connections

- **Ember prefers UDP port 45760 for matches.** It used a random port on every start, so there was nothing to forward in a router. A second copy on the same PC takes 45761 to 45763, and if all are busy Ember uses a random port as before. Ember still tries a direct connection first and falls back to the relay.
- **If Check connection says Relayed, you can forward the port.** Forward UDP 45760 to your PC in your router and allow `sf4-net.exe` in Windows Firewall. On strict routers this usually gives other players a direct path. Players whose internet provider shares one public address between many customers cannot forward a port and may stay relayed.
- **Check connection explains a Relayed result** and what can make it direct. The Direct/Relayed label is translated.

### Joining and rooms

- **Players with the same name can join each other.** Every install was named "Player" until you changed it, and a room turns away a second player with a name it already has. New and unchanged installs now get a name like "Player 4821". You can rename yourself in Settings as before.
- **A refused join says why,** for example "That player name is already in the room. Change it in Settings." or "This room is full."
- **Opening a room no longer says "Room control is recovering".** If it has not opened after about 30 seconds, Ember says so. While a room opens, the stop row reads **Stop creating** or **Stop joining**.
- **Players whose game crashed no longer stay in the room,** and hosting passes to someone who is still connected.
- **A finished match no longer blocks the next one at its table,** even if your profile could not be saved.
- **Leaving a room always finishes,** after at most 8 seconds.
- **Room buttons are kept through a brief reconnect** and go through once the room catches up. Chat can no longer cancel them, and only your latest choice counts: Queue then Unqueue leaves you out of line.
- **Battle slots are easier to read.** The fighter portraits sit at the outer edges and are larger, with the score alone in the middle.

### Matches

- **Both fighters play at the same input delay: the higher of their two Ready choices.** Separate delays gave the fighter with the lower one an advantage. Battle setup shows a **Match delay** row once both seats are filled.
- **Rollback sound fix.** Two identical sounds playing at once could cut each other off after a rollback.
- **Failed rollback saves end the match cleanly** instead of continuing on incomplete data.
- **The ping in the match display no longer shows a stale value** while the connection is interrupted.

### Launcher and updates

- **The launcher checks your Visual C++ runtime** before starting the game, since an older runtime made the game crash at every start. It offers to open the download page.
- **Starting Ember while it is already running says so,** with how to end Launcher.exe and SSFIV.exe in Task Manager if you cannot see Ember.
- **An interrupted update is repaired at the next start** and can no longer lock you out of the game. Extracting a new package over an old folder is never undone by a leftover update record.
- **Update files no longer pile up,** and a finished update is never reported as rolled back.
- **Preflight accepts the downloaded checksum file** and other files of yours in the folder. A stray program file, such as a DLL from an older version, still fails the check.

### Character art

- **Pictures that fail to load are retried,** and portraits fall back to the packaged original-outfit picture when the game's own cannot be read. Failures are logged once as `Selection art ... unavailable`.

### Diagnostics

- **Help & About > Export diagnostics shows the UDP port in use.**
- `sf4e.log` records each match link's route and the helper's port, and more detail when a match fails to close.

## Testing

Players tested the v0.9.8-rc4 and rc5 builds, including a Linux (Proton) session with several rematches in one room. The build's automated tests (57) and the networking helper's tests pass. The room integration fixture currently times out intermittently on the build machine with or without this release's network change; it is not part of the release suite.

## Compatibility

Everyone in a room must use v0.9.8. A room checks that all games run the same build, so v0.9.8 cannot play v0.9.7 or the rc test builds.

Known limits under Linux (Proton or Wine): `preflight.cmd` cannot check the files, because Wine's PowerShell does nothing, and the in-app updater cannot extract a package. Update by extracting the new ZIP into a new folder.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-0.9.8` ZIP, run `preflight.cmd`, then `Launcher.exe`. Players on v0.9.7 can use the in-app updater or the upgrade ZIP. If Ember does not open after the in-app update, start it again.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
