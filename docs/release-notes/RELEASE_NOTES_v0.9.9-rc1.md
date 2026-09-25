# SF4 Ember Netplay v0.9.9-rc1 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It has everything in v0.9.8 plus the changes below. If you would rather not test, stay on v0.9.8, the current full release.

## Changes since v0.9.8

### Matches

- **You no longer end up controlling both characters after a disconnect.** When a match lost its connection mid-fight (the room closed, you left, or the other game froze or closed), the fight could keep running on your PC with your controller driving both sides. The fight now ends and you return to the room.
- **The room stays open when someone joins during a fight.** Two kinds of helper errors that only concern one match used to close the whole room, and a burst of room events could stop the helper. Both now leave the room open. We could not reproduce the report itself, so please tell us if a room still closes when someone joins mid-match.
- **A failed match start no longer leaves its network port open**, so the next match can prepare cleanly.
- **Your profile record is saved once per match**, not two or three times.

### Messages

- **Messages that networking is unavailable or could not start stay on screen in every language** until networking is back. In Spanish and Portuguese they disappeared after 30 seconds.
- **When the previous match fails to close, you now see that message first**, instead of a "Your Ready did not go through" message that did not explain it.

### Crash reports

- **When the game crashes, Ember now writes a crash record.** It is `sf4e-crash.log` (with `sf4e-crash.dmp`) in the same logs folder as `sf4e.log`. It names where the game crashed and holds the last log lines. Earlier builds closed with nothing in the log.
- **`launcher.log` records the game's exit code**, and `sf4e.log` records the game's memory use at the start and end of each match.
- **`sf4e.log` records the input delay each side applied** at the start of every match, so two players' logs show whether they matched.

## Compatibility

Everyone in the room, including spectators, must use this same package. A room checks that every game runs the same build, so rc1 cannot play v0.9.8.

## What to report

The launcher and updater still report 0.9.8, so that the updater moves you to the real 0.9.9 when it ships.

This build exists mainly to catch crashes. We especially want:

- **Yang's Super (Seiei Enbu) online.** If the game crashes or closes, send the whole logs folder.
- **Long sessions of fifteen or more matches**, since some games closed on their own after ten to fifteen matches.

After a session, send `sf4e-crash.log` if there is one, `sf4e.log`, the newest `session-*.log` and `launcher.log` (see [saving logs](../guides/SAVING_LOGS.md)), from both players if you can, and say roughly when it happened.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-v0.9.9-rc1` ZIP into a new, empty folder (do not extract over an older version), run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
