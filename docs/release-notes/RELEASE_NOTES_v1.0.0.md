# SF4 Ember Netplay v1.0.0

v1.0.0 adds controller and keyboard table controls, spectator lock-in and fixes for saved replays, rematches, rejoining rooms and long-session crashes.

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes since v0.9.9

### Rooms and matches

- **Choose seats, queue or watch from a table card.** Table options brings together Ready, fighter, Ultra, appearance, fighter options, P1's stage, rules, input delay, Check connection and Leave seat.
- **Spectators can lock in to watch consecutive games.** Choose Lock in to watch in Table options. The next start waits up to 10 seconds for a locked-in spectator still leaving the previous game. Either fighter can cancel by taking Ready back. Playback can also catch up when it falls behind.
- **Connection checks no longer block later rematches.** Your result also stays available when the other player runs a check.
- **New online replays track rollback correctly.** Replay recording now rewinds with the match, so corrected frames are not recorded twice. Existing damaged replays stay broken.
- **Fixed a cause of long-session and rematch crashes.** Updating a connection message could cause heap corruption and exit code `0xC0000374`.

### Menus and controls

- **Pad and keyboard shortcuts follow the room's controls.** Xbox pad: A on your seat toggles Ready, B leaves your seat or queue, X opens Fighter, Y opens Table options and View opens Chat. Keyboard: F opens Fighter, T opens Table options, C opens Chat, Escape goes back and Delete leaves your seat or queue. Leaving asks first if you would lose a set score or give the seat to someone queued.
- **Navigation stays focused.** Prompts follow the last keyboard or pad input, and DirectInput devices retain mapped LP/LK prompts. A resting mouse pointer no longer takes the highlight away. Back from Home hides Ember without leaving the room. Keyboard and mouse input now reaches the menus in order.
- **Change fighter, Ultra, costume and color from your seat.** Fighter select starts at your current fighter and leads to Ultra cards with their inputs. Appearance leads through costume and color cards back to the table. Left and Right change Ultra or color directly on their rows.
- **Fighter options, stage and rules are on the table page.** Change personal action, win quote, handicap and edition there. Only P1 changes the stage. The host edits rounds, round time and Edition Select; applying changed rules clears Ready.
- **Input delay has one row.** Select applies the recommendation; Left and Right choose 0 to 10 frames. Leave room and Replace room remain on the room board.

### Spectators, invitations and room recovery

- **Failed watching no longer silently skips later games.** You stay set to watch, but a failed stream or setup clears lock-in so you can choose it again. Spectators return sooner after a finished game, and stream-loss and opponent-disconnect messages remain visible.
- **Leaving and rejoining handle more room failures.** Rejoining after a crash no longer loops or leaves a ghost seat. A player dropping while someone else leaves no longer freezes the room. Leaving the queue stops automatic watching unless you choose to watch.
- **Guests can copy the room invitation.** Refused invitations explain whether they expired, came from a different package or were pasted incompletely.
- **Rooms stay joinable after an hour.** The invitation every member shows is renewed while the room is open, so a fresh copy always works. A copied invitation still expires within an hour.
- **A former host can rejoin.** After handing host to another player and leaving, copy the room's invitation again. A copy taken before you left points at your own game; it now says so instead of reporting an unreachable host.

### Stages, languages and appearance

- **P1 can choose the Random stage pool.** Open Fighter select > Stage > Random stage pool to include or skip stages. The pool is saved, and at least one stage must remain included.
- **All USF4 interface languages plus Latin American Spanish are available.** Automatic follows the game's language, with a Windows fallback when the game uses English. Translations are drafts and still need native-speaker review.
- **Every costume color has a preview.** Alternate costumes show all their colors, and the preview images take less package space. Japanese, Korean and Chinese names and chat display supported characters.

### Connection details, training and sound

- **The Online screen shows relay region and network type.** Connection checks explain relay use and why a direct connection likely failed. Join and room-creation messages distinguish relay trouble, an unreachable host and a dropping link. Player cards and the match HUD show wired or Wi-Fi only when identified, otherwise Connection unknown.
- **The training frame meter sits above the super meters.** The F6 Training controls prompt is clickable, and unavailable measurements show a reason.
- **The announcer calls when your opponent readies first.** Settings > Interface has an on/off setting, volume and Test challenger call. The game's voice volume still applies. The call plays once per Ready, with a short wait before another call after unreadying and readying again.

### Stability, updates and crash reports

- **Rollback restores sound state and sprite animation slots correctly.** A failed restore returns you to the room instead of closing the game. If the HUD cannot be restored exactly, the match ends with a message.
- **Rollback memory is checked and old saves are freed without touching the game.** Added after a heap corruption crash (`0xC0000374`) while an old rollback save was freed, whose first cause is not confirmed. Freeing a save no longer writes into the running game, and the save system tracks who owns each saved block. Anything unsafe is skipped and the match ends with a message instead of corrupting memory. Each match ends with a `SaveSlots [battle_close_exit]: guards` line in `sf4e.log`.
- **The overlay is no longer freed while it is drawn.** A display reset (for example Alt+Tab in fullscreen) recreated the overlay while the render thread could still be drawing it.
- **Very long rounds stop replay recording when the round's recording buffer fills.**
- **The game starts even if `sf4e.log` cannot be opened.**
- **Double-clicking `Updater.exe` opens Updates.** A found update appears first with its version; Select installs it. The in-game Exit and open updater row explains the next step.
- **`preflight.cmd` is an optional extraction check.** It changes nothing, shows PASSED or FAILED with the next step, and waits for a key.
- **The launcher saves crash dumps and gives clearer reporting instructions.** It keeps five regular dumps and two larger heap dumps, which can be several hundred MB. `launcher.log` and `sf4e.log` identify their build at startup.

## Testing

The full build passes all automated tests. A two-PC rematch series of 17 matches over 48 minutes ran cleanly, with no crash, and recovered from an 8 second connection drop mid-match.

Thanks to everyone who tested the 1.0.0 rc builds.

## Compatibility

Everyone in a room needs v1.0.0, including spectators. v0.9.9 and the 1.0.0 rc builds cannot join a v1.0.0 room.

On Linux (Proton or Wine), `preflight.cmd` cannot check the files because Wine's PowerShell does nothing, and the in-app updater cannot extract a package. Extract the full ZIP into a new folder.

## How to update and play

On Windows, v0.9.9 installs can use the in-app updater. It follows the GitHub release marked Latest and downloads the full ZIP. You can also extract the [full ZIP](https://github.com/Confetti3/SF4-Ember-Netplay/releases/download/v1.0.0/sf4-ember-netplay-1.0.0.zip) into a new, empty folder. Linux players and rc testers should use the full ZIP in a new folder.

The [v1.0.0 release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.0.0) includes `sf4-ember-netplay-1.0.0.zip` and its `.sha256` checksum.

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam and the latest [Microsoft Visual C++ x86 runtime](https://aka.ms/vc14/vc_redist.x86.exe). Extract the entire ZIP, optionally run `preflight.cmd`, then run `Launcher.exe`. If Ember does not open after an in-app update, start it again.

Host copies a private invitation; Join pastes it. Choose a table, take a seat and have both players select Ready. Graphics and button mappings use the native game Options menu. Keep invitations private.

See the [player guide](https://github.com/Confetti3/SF4-Ember-Netplay/blob/v1.0.0/docs/guides/USER_NETPLAY.md), [Training Lab guide](https://github.com/Confetti3/SF4-Ember-Netplay/blob/v1.0.0/docs/guides/TRAINING_LAB.md) and [troubleshooting](https://github.com/Confetti3/SF4-Ember-Netplay/blob/v1.0.0/docs/guides/TROUBLESHOOTING.md).

## What to report

Report crashes, freezes, failed rematches, spectator lock-in problems, incorrect replay playback, or controls and translations that feel wrong in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Say roughly when it happened, what you were doing and which controller or keyboard controls you used.

Include `sf4e.log`, the newest `session-*.log`, `launcher.log` and `sf4e-crash.log` if present, from `%APPDATA%\sf4e\logs`. Logs from both players and affected spectators help. A `MementoGuard:` line, or a nonzero count on a `guards` line other than `releases` and `tracked_keys`, is worth reporting even when nothing seemed wrong. See [saving logs](https://github.com/Confetti3/SF4-Ember-Netplay/blob/v1.0.0/docs/guides/SAVING_LOGS.md).

Send crash dumps privately. They can contain invitations, player names and chat.
