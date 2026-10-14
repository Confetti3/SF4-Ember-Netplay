# In-game self-test

Some of what Ember does cannot be reached by a unit test: what the frame
meter shows for a move the game itself plays, or whether a saved position
comes back. The in-game self-test runs those in the game and says what
happened.

## Running it

The self-test is built only when CMake is configured with
`-DSF4E_SELFTEST=ON`; a build without it, as every package is, has none of it.

```powershell
scripts\run-selftest.ps1 -PackageDir <a built package or stage folder>
```

The game has to be closed, and Steam running with an account signed in. The
script:

1. copies the account's whole save folder to
   `%APPDATA%\sf4e\selftest-backups\<time>` and stops if it cannot;
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
  `ClassifyMeter` in `FrameMeter.hxx`);
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
`-Test` check.
