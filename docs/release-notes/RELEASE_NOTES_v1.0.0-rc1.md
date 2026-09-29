# SF4 Ember Netplay v1.0.0-rc1 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is the first public Ember 1.0 test build, published as a GitHub pre-release for testers.** It has everything in v0.9.9 plus the changes below. If you would rather not test, stay on v0.9.9, the current full release. v0.9.9 stays marked Latest, and the updater only follows Latest.

## Changes since v0.9.9

### Rooms and spectators

- **Spectators can lock in to watch consecutive games.** Choose Lock in to watch in Table options. If you are still leaving the previous game when both fighters ready, the next start waits up to 10 seconds for you. Either fighter can cancel that start by taking Ready back.
- **Spectators can catch up when playback falls behind.** Watching no longer fails just because playback falls about a second behind. Spectators also return to the room sooner after a finished game.
- **A failed watch no longer makes you silently miss every later game.** You stay set to watch later games, but your lock-in clears if your stream or setup fails. You can lock in again. Stream lost and opponent disconnected messages remain visible after the game closes.
- **Rematches no longer get blocked by an earlier connection check.** This fixes a cause of matches stopping after a few games. Your connection-check result also stays available when the other player runs a check.
- **Leaving and rejoining handle more room failures.** Rejoining after a crash no longer loops indefinitely or leaves a ghost seat. A player dropping while someone else leaves no longer freezes the room. Leaving the queue no longer leaves you watching future games unless you choose to watch.
- **Copy invitation now works for players who joined the room.** You can copy and share the room's invitation without being its creator.

### Menus and controls

- **The room now follows controller-first controls.** Select a table card to choose a seat, join its queue, watch or open Table options. On an Xbox pad, A on your own seat readies or unreadies you. B leaves your seat or queue place, asking first only if you would lose a set score or give the seat to someone queued. X opens Change fighter, Y opens Table options and View opens Chat.
- **The rest of the overlay follows the room's controls.** Dialogs use consistent Select and Back actions, with prompts for the current screen. A resting mouse pointer no longer takes the highlight away from your controller. Back from Home hides Ember without leaving your room. DirectInput devices retain their mapped LP/LK prompts.
- **P1 can choose which stages Random uses.** Open Fighter select > Stage > Random stage pool. Select a stage to skip it or put it back. The pool is saved for P1, and at least one stage must stay included.

### Languages and appearance

- **Ember now offers all USF4 interface languages, plus Latin American Spanish.** Spanish from Spain, French, Italian, German, Dutch, Polish, Czech, Russian, Japanese, Korean and Simplified Chinese join English, Brazilian Portuguese and Latin American Spanish. Automatic follows the game's language, with a Windows fallback when the game uses English. Translations are drafts and still need native-speaker review.
- **Every costume color now has a preview.** Alternate costumes no longer show only a small selection of their colors. The preview images also take less space in the package.
- **Japanese, Korean and Chinese names and chat now display supported characters.** A crash caused by the interface fonts using too much memory has also been fixed.

### Connection details

- **The Online screen shows your relay region and network type.** Regions include US East, US West, Europe and Asia Pacific. Network types include Open, Strict NAT and UDP blocked. Player cards and the match HUD also show wired, Wi-Fi or unknown connection indicators.
- **Connection messages give more detail about where a problem occurred.** A relayed Check connection result names the relay region and explains the likely reason a direct connection failed, including whose side it concerns when known. Failed joins and room creation distinguish relay trouble, an unreachable host and a dropping link. These details show or share region names and network types, never addresses.

### Training

- **The training frame meter now sits above the game's super meters.** It no longer covers them. The F6 Training controls prompt is also clickable, and unavailable measurements show a reason. See the [Training Lab guide](../guides/TRAINING_LAB.md).

### Sound

- **The game's announcer calls when your opponent readies before you.** You hear "Here comes a new challenger!" Settings > Interface has an on/off setting, volume and Test challenger call. Its volume is a share of the game's voice volume, so the game's voice slider still applies. It calls once per ready, with a short wait before another call if your opponent unreadies and readies again.

## Compatibility

Everyone in the room, including spectators, must use this same package. A room checks that every game runs the same build, so v1.0.0-rc1 cannot play v0.9.9 or any earlier package. This includes the unpublished local 1.0 test package some players tried earlier today.

## What to report

The launcher and updater still report 0.9.9 during 1.0 testing, so that the updater moves you to the real 1.0 when it ships.

We especially want:

- **Rooms of 3 or more with spectators.** Try Lock in to watch across several rematches. Tell us whether spectators watch every game, return to the room and can watch again after a failed stream.
- **The controller-first room.** Try choosing seats, queuing, watching, Ready, Back and the room shortcuts. Tell us which controller you used and where a control or prompt felt wrong.
- **Copy invitation from a player who joined.** Have a guest copy the invitation and check whether another player can use it to join.
- **The challenger call in real rooms.** Tell us whether it is audible, whether its volume setting works and whether it repeats without a new Ready.
- **Relay regions and network types.** Tell us whether yours look right and whether a relayed connection's explanation matches both players' setups.
- **Random stage pool over several rematches.** Exclude a few stages as P1. Tell us whether Random stays within the pool and everyone sees the same stage.
- **Rematches after Check connection.** Run a check between games and tell us whether both players can keep starting matches.
- **Crashes and long sessions.** Report crashes, freezes or problems that appear after playing for a while. If the game closes with the "game exited with an error" screen, include `launcher.log`.

After a session, send `sf4e-crash.log` if there is one, `sf4e.log`, the newest `session-*.log` and `launcher.log` (see [saving logs](../guides/SAVING_LOGS.md)), from both players and affected spectators if you can, and say roughly when it happened.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-v1.0.0-rc1` ZIP into a new, empty folder (do not extract over an older version), run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for test builds.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Choose a table and take a seat, then both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
