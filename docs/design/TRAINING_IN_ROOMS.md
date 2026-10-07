# Waiting in Training while in a room

A member of a room can spend the wait for a seat or an opponent in the game's
Training mode. When another fighter sits down opposite them, Ember takes them
out of the battle and back to their table, where they ready.

This note says what was built, what it rests on in the existing code, and what
is left for a decision.

## What a player sees

1. On the room screen, **Training** (enabled at the main menu, with no Ready
   given, no game of theirs starting and nobody opposite them yet). The menu
   closes and the game goes to the Training fighter select. The room stays
   joined.
2. They train. Their place in the room does not change: queued, or seated
   alone.
3. A fighter sits down opposite them. Over the battle: the announcer's
   challenger call (unless the ready sound is off) and the banner "A NEW
   WARRIOR HAS ENTERED THE RING!" for two seconds. Then the battle ends as the
   pause menu's "exit to main menu" ends it.
4. At the main menu the Ember menu opens on their table. The Ready row says
   how many seconds are left: 15.
5. They press Ready and the match starts the usual way. If the 15 seconds run
   out, their seat is given up, by the action the Leave seat row sends, and a
   notice says so.
6. With **Ready me when called from Training** on (Settings, off by default)
   they are readied at once instead.

## What it rests on

Nothing in the room authority, the wire format or the server changed.

- Being in Training while a member was already possible: the menu can be
  hidden and Training picked in the game's own menu. `matchWaitsForMenu`,
  `EntryGate` and the "return to the menu" notice exist for that. This feature
  adds a way in from the room screen and a way out when someone is waiting.
- **The rule** is `room::TrainingCall` (`src/session/TrainingCall.hxx`), a
  class beside `ReadyChime` and tested the same way
  (`src/tests/training_call_test.cxx`). It reads the room snapshot and four
  facts (in Training, at the main menu, a Ready is possible, the player wants
  to be readied) and returns one step at a time: Call, Open, Ready, Forfeit.
- **The runtime** (`CallOutOfTraining` in `sf4e__NetplayRuntime.cxx`, beside
  `CallOutOpponentReady`) turns the steps into things that exist:
  - Call: a `Leave` command to the training runtime.
  - Open and Ready: two sequence numbers in the snapshot, as
    `opponentChangeSequence` is one. The shell acts on them, because a Ready
    carries the selection the shell holds.
  - Forfeit: a `RoomAction` of kind `Unqueue` for the player's table through
    `SubmitRuntimeCommand`, stamped with the room's epoch and revisions as the
    room panel stamps it.
- **The Ready** is the shell's own `Send(CommandKind::Ready)`. It goes through
  the same dispatch as a pressed Ready, so every refusal and every parked
  retry applies to it.
- **Leaving the battle** (`TrainingRuntime.cxx`): the battle system's exit
  type is set to `BET_PAUSE_TOMAINMENU` and its ready state to `RS_ISLEAVING`,
  only once the fight is running. The game's own teardown follows, and
  `training::CloseBattle` clears the lab's state as on any exit.
- **Entering Training from the room** uses `fMainMenu::RequestTraining`, the
  same request the Home row makes, without the `StartOffline` command that row
  rides on: that command ends the room.

## The rule in full

- A call is made only to a player who is **in a Training battle** and
  **seated** with another fighter in the other seat, at a table that is idle
  or waiting, and who has not readied. A queued member or a spectator is not
  called.
- The window to ready starts when they reach the main menu, not when they are
  called, so the time the battle takes to close is not taken from them.
- The window ends without a forfeit when they ready, when the opponent leaves
  or is replaced, or when the table stops being theirs.
- A call whose battle has not left after 20 seconds is forgotten. Nothing is
  taken from the player for that; if they are still in Training they are
  called again.
- Nobody is called at the main menu. A player who never went to Training has
  no window and no forfeit: readying in a room is as it was.

## What was checked

- `TrainingCallTest`: every branch of the rule.
- `ShellJourneyTest`: the room's Training row sends no room command; a player
  called back lands on their table; a Ready is sent once, only when the
  runtime allows one, and not after the window closed.
- The whole test suite and the localization test.
- In the game, by hand, with a test key in place of a second player: the call
  is heard in the battle, the battle closes about half a second after it is
  told to, the game is at the main menu, and there is no crash record.

## What was not

- **Two players in one room.** The call from a real opponent sitting down, the
  menu opening on the table, the Ready and the forfeit have not been run end
  to end in the game.
- A player who sits in Training's fighter select is not in a battle and is not
  called until one starts. Their opponent waits, as an opponent waits today
  for a member who is away from the menu.

## For a decision

1. **The announcer in a battle.** `PlayChallengerCall` keeps the call to the
   main menu on purpose. The `Leave` command plays the same cue inside a
   Training battle. It worked in the trial above, but it goes against that
   note. Dropping it leaves the banner alone.
2. **The forfeit.** It is local: the player's own client gives the seat up. A
   client that does not do it leaves the opponent waiting, as today. Making
   the room enforce it would be a change to the authority.
3. **Showing "in Training" to the room.** Not built. Member status is derived
   by the authority from table membership, and `from_json` refuses a status
   above `WatchingNext`, so a new status value is a wire change. A boolean on
   the member, read with a default like `spectatorLocked`, and an action to
   set it, appended like `LockSpectating`, would avoid that.
4. **The banner** is drawn by Ember. The game's own is `ui\intrusion`, shown
   by its Arcade fight-request classes, for which there are no bindings.
