# SF4 Ember Netplay v1.0.0-rc3 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is an Ember 1.0 test build, published as a GitHub pre-release for testers.** It has everything in v1.0.0-rc2 plus the changes below. If you would rather not test, stay on v0.9.9, the current full release. v0.9.9 stays marked Latest, and the updater only follows Latest.

## Changes since v1.0.0-rc2

### Costumes in rooms

- **You can change costume and color without leaving the room.** Table options has a new Appearance row under Ultra Combo. Select opens the costume cards, picking a costume goes straight on to its colors, and picking a color brings you back to the table. Left and Right on the row change the color in place.
- **Fighter select follows the same order.** Picking a costume goes on to its colors, and picking a color returns to Fighter select.

### Stability

- **Typing and mouse input no longer race the overlay.** SF4 handles keys and the mouse on a different thread from the one that draws Ember's menus. Input now reaches the menus in order on the drawing thread. This is a likely cause of crashes while typing a name, and possibly of the mid-match crash reported on rc1. We have not confirmed that yet, so please keep sending crash logs.
- **A failed rollback returns you to the room instead of closing the game.**
- **Rollback restores the game's sound state correctly.** A restore could leave two of its internal lists joined together.
- **The game starts even if its log file cannot be opened.** It used to close at startup when `sf4e.log` was read-only or held open by another program.

### Updates and the package check

- **Double-clicking `Updater.exe` opens the Updates window.** It used to exit without a window.
- **A found update is listed first, with its version, and highlighted.** Select installs it instead of checking again. The in-game Exit and open updater row says what happens next.
- **`preflight.cmd` explains itself.** It says it is an optional check that every file extracted completely, that it changes nothing and that it is not the updater. It then shows PASSED or FAILED with the next step and waits for a key. The [player guide](../guides/USER_NETPLAY.md) has a short Updating section.

## Compatibility

Install the complete v1.0.0-rc3 package on both machines. Both players need rc3 to play each other.

## What to report

All 79 automated tests and the helper network tests pass on the full build, and an independent code review approved it. The developer checked the new menus and the costume flow in game on one PC. We still need these tests:

- **Typing a name.** If typing ever closed your game on an earlier build, try it on rc3 and tell us whether it still happens.
- **A two-PC rematch series.** Play back-to-back rematches, ideally with a spectator, and report any crash or other problem.
- **Changing appearance at a table** while someone else is in the room.
- **Crash logs.** If the game crashes, send the `%APPDATA%\sf4e\logs` folder, including any crash dump.

## Install and play

Extract the complete rc3 package into a new, empty folder on each machine and run `Launcher.exe`. Running `preflight.cmd` first is optional.

See the [player guide](../guides/USER_NETPLAY.md) for play instructions.
