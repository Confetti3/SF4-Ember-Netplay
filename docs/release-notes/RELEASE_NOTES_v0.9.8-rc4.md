# SF4 Ember Netplay v0.9.8-rc4 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It has everything in rc3 plus the fixes below. If you would rather not test, stay on v0.9.7, the current full release.

## Changes since v0.9.8-rc3

### Linux (Proton and Wine)

- **The room should no longer close about 30 seconds after a match.** At match end the networking helper could stop answering under Wine, and the room then closed on `match_teardown_timeout`. The helper now stays responsive when the game closes its match connection.
- **The launcher starts again under Wine.** rc3's Visual C++ runtime check rejected Wine's own runtime. Wine's runtime is now accepted; a Microsoft runtime installed into a Wine prefix is still checked.

### Matches

- **Both fighters play at the same input delay: the higher of their two Ready choices.** Separate delays gave the fighter with the lower one an advantage. Battle setup shows a **Match delay** row once both seats are filled. It reads "At least" your own value until your opponent readies, then shows the delay both of you will play at.

### Rooms

- **Opening a room no longer says "Room control is recovering".** A room that is still being created or joined is not in recovery, and no longer offers to replace a room you never joined. If it has not opened after about 30 seconds, Ember says so; check your connection, or stop and try again.
- **The stop row is named.** While a room opens, the row reads **Stop creating** or **Stop joining**, so its confirmation no longer shows two buttons both labelled Cancel.

### Launcher and updates

- **Starting Ember while it is already running now says so.** A second start used to do nothing at all. It now tells you, and how to end Launcher.exe and SSFIV.exe in Task Manager if you cannot see Ember. A start from a Discord invite stays quiet, because the running copy handles it.
- **An interrupted update can no longer lock you out of the game.** An update record that can never be restored is reported once, kept as `.ember-update-transaction-v1.json.failed`, and later starts go to the game.
- **Extracting a package over an old folder is not undone.** If a leftover update record was found in a folder you had since extracted a new package into, the launcher could restore the older files over it. It now leaves your files alone.
- **Preflight accepts the checksum file.** `preflight.cmd` used to fail when the downloaded `.zip.sha256` (or another file of yours) was in the folder. Such files are now reported and ignored. A stray program file, such as a DLL left from an older version, still fails the check.

### Character art

- **Pictures that fail to load are retried** after 1 and 2 seconds instead of staying blank until you restart the game.
- **Portraits fall back to the packaged original-outfit picture** when the game's own portrait cannot be read, for the fighters that have one.
- **Failures are logged once** as `Selection art ... unavailable` (or `... uses a fallback`) with the file and the reason: in `sf4e.log` in game, and in `launcher.log` on the launcher's recovery screen.
- **Small portraits use less memory.** Member rows and player cards draw from the small portrait instead of the full-size one.

### Diagnostics

- With `SF4E_ROLLBACK_DIAGNOSTICS=1`, the `RollbackDiag` summary also reports `sound_sync` and `limiter_wait`. A `limiter_wait` near zero means the PC cannot keep 60 fps. See [saving logs](../guides/SAVING_LOGS.md) for how to turn it on.
- The game's frame limiter takes exactly the same path as in 0.9.6 unless pacing needs to shift a frame, so version comparisons are fair.

## Compatibility

Everyone in the room, including spectators, must use this same package. A room checks that every game runs the same build, so rc4 cannot play rc3, rc2 or v0.9.7.

## What to report

The launcher and updater still report 0.9.7, so that the updater moves you to the real 0.9.8 when it ships.

After a session, send `sf4e.log` and the newest `session-*.log` from the same folder (see [saving logs](../guides/SAVING_LOGS.md)), from every player in the room if you can. If the room closes on you, the lines just before `Room: closing` matter most.

Linux (Proton) players: please tell us whether rematches now work and the room stays open after a match. If it still closes, lines that start with `Helper:` or `Match teardown:` are the ones we need.

Everyone: tell us whether the shared match delay feels right, and send any `Selection art` lines if character pictures are missing.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-v0.9.8-rc4` ZIP into a new, empty folder (do not extract over an older version), run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
