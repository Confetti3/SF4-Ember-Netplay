# SF4 Ember Netplay v1.0.0-rc2 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is an Ember 1.0 test build, published as a GitHub pre-release for testers.** It has everything in v1.0.0-rc1 plus the changes below. If you would rather not test, stay on v0.9.9, the current full release. v0.9.9 stays marked Latest, and the updater only follows Latest.

## Changes since v1.0.0-rc1

### Crash reporting and stability

- **The launcher can now save a crash dump when the game closes mid-match.** A tester hit heap corruption during back-to-back rematches, and that kind of crash previously left no crash record. The game now records it, and the launcher saves `sf4e-crash-<date>-<time>-<ms>-<pid>.dmp` in `%APPDATA%\sf4e\logs`. It keeps the newest five dumps. The cause of the crash is still unknown.
- **The launcher gives clearer instructions after a crash.** It says the game crashed and names the files to send. The [player guide](../guides/USER_NETPLAY.md) also explains how to turn on Windows LocalDumps for crashes that happen before the game's own crash handlers run.
- **Several paths have been hardened while we investigate.** The challenger call plays only at the main menu when no match is running. A font atlas rebuild triggered by unusual characters in player names can no longer write past its buffer. Text fields cap their drafts, and there are more log lines around rematches and the overlay.

### Menus and controls

- **Keyboard players have room shortcuts.** F opens Fighter, T opens Table options and C opens Chat, matching X, Y and View on an Xbox pad. Press the same key again to return to the room board. Button prompts follow whichever you pressed last, keyboard or pad.
- **Keyboard Back and Select are more consistent.** Escape goes back even when your own seat is selected. Delete leaves your seat or queue place. Space and Backspace also select and go back, and numpad Enter selects, except while you are typing in a text box. A pad's B still leaves in one press.
- **Changing fighter now starts at your current fighter.** Picking a fighter goes straight to its Ultra. Ultras appear as photo cards with their inputs, and picking one returns you to the room. You can also change Ultra Combo with Left and Right on the fighter select page or the table page without opening another page.
- **Table options are simpler.** The page shows Ready, Change fighter, Ultra Combo, one Input delay row, Check connection, then the table's rules. The delay detail shows the recommended and match delay. Select applies the recommendation; Left and Right choose 0 to 10 frames.
- **The host edits rules on the table page.** Rounds, Round time and Edition Select can be changed there. Apply rules appears only when something differs from the table's rules, and applying clears Ready. Other players see the rules on one line. Leave room and Replace room are on the room board only.

### Rooms and connection details

- **Refused invitations explain why.** The message says whether the invitation expired after its one-hour lifetime, came from a different package or was pasted incompletely.
- **Player connection labels are more careful.** Wired or Wi-Fi appears only when the link can be clearly identified. Otherwise the label says Connection unknown.

## Compatibility

Install the complete v1.0.0-rc2 package on both machines. Both players need rc2 to play each other.

## What to report

All 77 automated tests pass on the full build, and an independent code review approved it. The developer checked the new menus in game on one PC. We still need these tests:

- **A two-PC rematch series on this build.** Play back-to-back rematches and report any crash or other problem.
- **The seated table page with a second player.** Check its controls, prompts, delay row and rules while someone else is in the room.
- **Crash logs.** If the game crashes, send the `%APPDATA%\sf4e\logs` folder, including any new crash dump. The dump is needed to find the cause of the mid-match crash.

## Install and play

Install the complete rc2 package on both machines as with rc1. Extract each copy into a new, empty folder, then run `preflight.cmd` and `Launcher.exe`.

See the [player guide](../guides/USER_NETPLAY.md) for play instructions and Windows LocalDumps setup.
