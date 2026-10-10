# In-game self-test

Some of what Ember does cannot be reached by a unit test: Steam's own file
writes, the game's replay table, its battle log. The in-game self-test runs
those in the game itself and says what happened.

## Running it

```powershell
scripts\run-selftest.ps1 -PackageDir <a built package or stage folder>
```

The game has to be closed, and Steam running with an account signed in. The
script:

1. copies the account's whole save folder to
   `%APPDATA%\sf4e\selftest-backups\<time>` and stops if it cannot;
2. starts the package's `Launcher.exe` with `SF4E_SELFTEST=replays`;
3. sends one Enter key to the game's window when the test says the title
   screen is up, and gives the focus back to the window that had it;
4. waits for the game to close itself, prints each step and exits with 0
   when every step passed.

A run takes under a minute. Apart from the few seconds around the Enter key
the game needs no focus: in a test run it counts its own window as in front.
Sound follows the Play in the background setting.

## What the replays test does

It drives the game through the requests the Replays screen sends
(`RunReplayRequest`), with the smallest Ember replay of the archive:

- an import whose third write is made to fail: the import has to report the
  file failure, and every file an import may touch has to hold the bytes it
  had before;
- Add: exactly one match slot changes and holds the replay, and the replay is
  not marked Watched;
- Watch now: one more slot takes the replay, the battle log plays it, and the
  main menu comes back with Ember on the Replays screen; the replay is marked
  Watched once. The test leaves the replay after thirty seconds, since a
  replay ends on a menu that waits for the player.

It writes into the signed-in account's match slots, as Add to game does. The
replay a slot held is in the archive first, as always, but after enough runs
the game's own list holds the test's replay in every match slot.

## How it is built

- `sf4e__NetplayRuntime__SelfTest.cxx` is the whole test: one step a runtime
  tick, each with an end. Without `SF4E_SELFTEST` nothing in it runs and
  nothing is installed.
- The title screen learns from its Start which device, and so which profile,
  plays. A Start that comes from no device makes the game find no save, so
  the test never answers the title screen itself: the script sends a real
  key, once for each arrival at the title screen and three times at most. A
  message the game shows there is never answered.
- `InstallSelfTest` makes one edit of the kind Background play makes
  (`focus_gate::CallThrough`): the frame's question whether its window is in
  front (`Dimps::App::activeFocusCheck`) is answered with the game's own
  window. With that off, the game's events stand still behind another
  window.
- `replaystore::FailWriteForTest` is the one seam in shipped code: the write
  after so many succeed fails once.

## Adding a test

Give it a name in `TickSelfTest`, its own stages, and a line in this file.
Keep to requests the interface already sends, and to reading: a test that
needs the game to do something a player cannot ask for is testing something
else.
