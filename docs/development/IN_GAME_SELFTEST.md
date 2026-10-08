# In-game self-test

Some of what Ember does cannot be reached by a unit test: what the frame
meter shows for a move the game itself plays, or whether a saved position
comes back. The in-game self-test runs those in the game and says what
happened.

## Running it

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
   when every step passed.

A run takes about a minute. Apart from the few seconds around the Enter key
the game needs no focus: in a test run it counts its own window as in front.
Sound follows the Play in the background setting.

## What the training test does

It enters an offline Versus battle of Ryu against Ryu the way a room's match
is entered, with no menu, lets the training lab into that battle, and then
uses only what the lab's own controls send (`training::Submit`):

- moves typed in numpad notation are loaded into a recording slot and played
  by Player 1. For Ryu's crouching LP the meter's numbers have to be the
  frame data's, startup 3, active 2 and recovery 7, and its cells 3, 2 and 6
  (the last recovery frame has no cell; see `ClassifyMeter` in
  `FrameMeter.hxx`). Four more moves are played and what the meter showed
  for them is reported, not judged;
- Save position, a forward dash, Reset position: Player 1 has to stand where
  the position was saved, and the dash has to have moved the fighter, or the
  reset proves nothing.

It plays an offline battle and writes nothing to the saves. The copy is made
all the same, since the run goes through the title screen of the signed-in
account.

## How it is built

- `sf4e__NetplayRuntime__SelfTest.cxx` is the whole test: one step a runtime
  tick, each with an end. Without `SF4E_SELFTEST` nothing in it runs.
- The title screen learns from its Start which device, and so which profile,
  plays. A Start that comes from no device makes the game find no save, so
  the test never answers the title screen itself: the script sends a real
  key, once for each arrival at the title screen and three times at most. A
  message the game shows there is never answered.
- The game counts its window as in front only in a test run.
  `BackgroundPlay::KeepActiveForTest` has the edit Background play already
  makes of the frame's question (`Dimps::App::activeFocusCheck`) answer with
  the game's own window, as it does while a replay is exported. With that
  off, the game's events stand still behind another window. No code is
  edited for the test and no hook is added.
- `training::AllowOfflineVersusForTest` is the other seam in shipped code:
  the lab runs in a Training battle only, and a test run lets it into the
  offline Versus battle the test starts.

## Adding a test

Give it a name in `TickSelfTest`, its own stages, and a line in this file.
Keep to requests the interface already sends, and to reading: a test that
needs the game to do something a player cannot ask for is testing something
else.
