# In-game self-test

Some of what Ember does cannot be reached by a unit test: what the frame
meter shows for a move the game itself plays, or whether a saved position
comes back. The in-game self-test runs those in the game and says what
happened.

## Running it

The self-test is built only by `scripts\build-current.ps1 -SelfTest`, which
configures CMake with `-DSF4E_SELFTEST=ON`. Every other run of the script
passes `-DSF4E_SELFTEST=OFF`, so a build directory that once held a self-test
build does not keep it, and its receipt (`build-provenance.json`) says
`selfTest`. `Assert-BuildReceipt` refuses a receipt that is not `false`, and a
cache that says ON, so `package-team.ps1` and the Nightly publisher will not
package or publish one. A self-test build stages into the same directory as
the build a package comes from: build it again without `-SelfTest` before
packaging.

```powershell
scripts\run-selftest.ps1 -PackageDir <a built package or stage folder>
```

The game has to be closed, and Steam running with an account signed in. The
script:

1. copies the account's whole save folder to
   `%APPDATA%\sf4e\selftest-backups\<time>` and stops if it cannot (the
   newest five copies stay);
2. starts the package's `Launcher.exe` with `SF4E_SELFTEST=training`;
3. sends one Enter key to the game's window when the test says the title
   screen is up, and gives the focus back to the window that had it;
4. waits for the game to close itself, prints each step and exits with 0
   when every step passed, 1 when one failed and 2 when the game left no
   result.

Each step's line starts with PASS or FAIL, and the last line is
`RESULT pass` or `RESULT fail`.

A run takes about a minute. Apart from the few seconds around the Enter key
the game needs no focus: a test run keeps running behind other windows, as a
watched match does. Sound follows the Play in the background setting.

## What the training test does

It enters an offline Versus battle of Ryu against Ryu the way a room's match
is entered, with no menu, lets the training lab into that battle, and then
uses only what the lab's own controls send (`training::Submit`). Playback
repeats by default, so it first turns Loop off, then:

- a move typed in numpad notation is loaded into a recording slot and played
  by Player 1. For Ryu's crouching LP the meter's numbers have to be the
  frame data's, startup 3, active 2 and recovery 7, and its bars have to
  draw 3, 2 and 6 cells (the last recovery frame has no cell; see
  `ClassifyMeter` in `FrameMeter.hxx`). While it plays, the boxes read from
  the game have to show where each fighter can be hit, and Player 1's attack
  box has to be live on the move's 2 active frames;
- the lab's record control is pressed three times: standby, recording for
  eight seconds, stop. The slot has to hold about that many frames, and its
  playback has to get into it. The HUD's recording and playback strip shows
  while this runs;
- Save position, a forward dash, Reset position: Player 1 has to stand where
  the position was saved, and the dash has to have moved the fighter, or the
  reset proves nothing.

The battle saves no replay and the lab keeps its slots in memory only, so
the test writes nothing to the saves. The copy is made all the same, since
the run goes through the title screen of the signed-in account.

## The room test

Two PCs play one game of a private room through the runtime's commands, the
ones the room's controls send:

```powershell
# PC 1
scripts\run-selftest.ps1 -PackageDir <stage> -Test room-host -Invite <file>
# PC 2, once the host's file has been copied to it
scripts\run-selftest.ps1 -PackageDir <stage> -Test room-guest -Invite <file>
```

The host opens a private room with a fixed input delay and one round, and
writes its invitation to the `-Invite` file. Copy that file to the other PC
(the guest waits up to three minutes for it). Each side sits, the host as
Player 1 and the guest as Player 2, readies as Ryu and waits for the match
to go live. The host then ends the game and both leave the room. Hosting,
the names the two sides take and the controller a room needs write Ember's
settings, so the script copies them first and puts them back after the run.

## The tournament test

Two PCs play one match of a local test bridge end to end: each creates an
Ember ID, trusts the bridge, links to its mock connection with a code and
plays a first-to-1 match through the runtime's own tournament flow. The
host's fighter attacks and the guest's stands still, so the game ends in the
host's win. The bridge side checks the match completed with the host as the
winner and that its signed result was delivered.

```powershell
scripts\run-tournament-selftest.ps1 -PackageDir <stage> -BridgeExe <ember-bridge.exe>
```

The script runs the bridge on this PC and the guest here, and the host on the
PC `-RemoteHost` names (ssh, with `-RemoteCheckout` holding this branch and
its staged build), through the scheduled task `-RemoteTask`. Each game keeps
its Ember ID and trusted services in a temp folder of its own, never the
player's, and its settings are put back after the run.
`docs/design/TOURNAMENT_SELFTEST.md` has the details, and the `-Site challonge`
mode, which connects each game's Challonge account in the browser and plays a
set of a real Challonge tournament.

Unlike the other tests it plays a game to its end. A finished game saves
its replay into the game's own slots (the ring of the last ten matches, which
would push out one of the player's), changes the replay index files (`LIST`,
`replays-swan.dat`) and `save.dat`, the account's record; runs made before the
script put the saves back were seen to change exactly those. The launcher
archives the replay into Ember's replay archive (`%APPDATA%\sf4e\replays`, by
way of `%LOCALAPPDATA%\sf4e\replay-outbox`). So when a tournament test's game
has closed, `run-selftest.ps1`
makes the save folder match its copy again (files the run added go, changed
ones come back), deletes the copy once the folder matches, and removes the
files the run added to the archive and the outbox. If the folder does not
match, the copy stays and the script says so. Steam Cloud syncs the game's
saves around its runs, so the restored folder is
seen as an ordinary local change; nothing else changes it in between.

## The replay test

`-Test replay` needs at least one Ember replay in the archive. From the main
menu, once the game's replay table is ready, it takes the archive's smallest
replay and sends the requests the Replays screen sends:

- an import whose third write fails (`replaystore::FailWriteForTest`): the
  import has to say the game's replays are as they were, and every save file
  an import may touch has to be;
- the same import again: exactly one match slot changes, it holds the replay
  and its checksum file is there, and the replay is not marked watched;
- Watch: the battle log plays it, the test leaves it after half a minute, and
  the main menu comes back to Ember. Watching imports it into one more slot
  and marks it watched once.

Unlike the training test it writes into the signed-in account's replay slots,
as an import does: the replay a slot held is in the archive first, as always,
and the saves were copied before the run.

## The title screen

The title screen learns from its Start which device, and so which profile,
plays. A Start that comes from no device makes the game find no save, so the
test never answers the title screen itself: the script sends a real key, once
for each arrival at the title screen and three times at most. The test logs
every change of the screen in front, and the key goes only while the newest
line still names the title screen. A message the game shows is never
answered.

## Adding a test

`src/sf4e/sf4e__SelfTest.cxx` is what every test shares: the result file, the
way to the main menu, the outcome and the closing. A test is a file of its own
beside it, like `sf4e__SelfTest__Training.cxx`, with its own stages
(`Steps<Stage>` in `sf4e__SelfTest.hxx`) and a start and a tick that the
shared part calls from the main menu on. It reports with `Say` and ends the
run with `Finish`. Its name goes into `selftest::Parse` and the script's
`-Test` check. A test that plays in a room takes the runtime's commands, its
setup, Ready as Ryu and the files two PCs pass lines through from
`sf4e__SelfTest__Online.hxx`, as the room and tournament tests do.
