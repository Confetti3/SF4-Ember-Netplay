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
   alone. A muted line under the game's timer says where they stand: the
   room's name, the table, "Queued 2 of 3", "Waiting for an opponent" or
   the opponent's name, and the room chat's unread count. It is read from
   the room snapshot the overlay already has, hides under any open window,
   and goes with the rest of the training HUD on F5.
3. A fighter sits down opposite them. Over the battle: the announcer's
   challenger call (unless the ready sound is off) and the banner "Your
   match is ready" for two seconds, with the opponent's name and fighter
   under it. Then the battle ends as the pause menu's "exit to
   main menu" ends it. **Go now** on the banner (Enter, or View on an Xbox
   pad; other pads are shown Enter) ends it at once instead, by the same
   countdown, so nothing after it changes. The F6 controls close for the
   call, and the banner's Enter is not passed to the game. View goes through
   the training pad gesture (`TrainingPad.hxx`), which takes a fresh View
   press for go now alone: it does not also reset or save the position, and
   Back with Start opens nothing during the call. A View already held when
   the call came ends as the tap or hold it began as. A DirectInput pad's
   Select keeps resetting and saving. Under the game's pause menu or Ember's
   menu, go now is not offered and Enter stays theirs. A Start pressed
   with View held is the gesture's during the call too, so it never opens
   the game's pause. The chips that open the F6 controls are hidden under
   the call. If the opponent gets up before the battle has left, the call
   ends: the banner goes, go now is no longer offered and the player stays
   in Training.
   The others at the table see "Calling {name} back from Training" on the
   table card until the player is back or readies.
4. At the main menu the Ember menu opens on their table. The Ready row says
   how many seconds are left: 15.
5. They press Ready and the match starts the usual way. If the 15 seconds run
   out, their seat is given up, by the action the Leave seat row sends, and a
   notice says so.
6. With **Auto-ready from Training** on (Settings, off by default)
   they are readied at once instead.

## What it rests on

The call itself changes nothing in the room authority, the wire format or the
server. Showing the others that a member is in Training does, in one commit
of its own: see "In Training, as the room shows it" below.

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
  `training::CloseBattle` clears the lab's state as on any exit. The wait is
  `training::LeaveCountdown`; go now is `Action::LeaveNow`, which only cuts
  that count to its last frame, so the battle leaves once, by this path,
  whichever ended the wait (`TrainingSessionTest`); `Action::Stay` cancels
  it when the call ends first.
- **The call** is `room::TrainingCall`, whose `Identity()` (room epoch,
  table, opponent, the Training battle and a serial no other call shares) is
  the one account of it, published in the snapshot. Its lifecycle runs in one
  order each tick (`TrainingCallInput.hxx: CallLifecycle`): a call that ended
  or was replaced by another (a new opponent, another room) has its countdown
  ended; the standing call's countdown is started, and the countdown belongs
  to that call's serial, so a replacement starts a whole one of its own and
  never inherits the old call's go now; only once that start is queued, and
  only while the battle the call was sent to still runs in Training (not
  leaving, not closed, not a later battle), is the call offered to go now's
  gate. Enter (in the window procedure) and View (in the pad gesture) are
  taken for go now where they arrive, only while nobody else has the press,
  and each taken press is a request tagged with the call. A request goes now
  only for the call still offered, and `LeaveNow` reaches only that call's
  countdown. The banner shows only while the countdown is the call's
  (`ControllerNavigationTest`, `Win32CursorTest`, `TrainingCallTest`,
  `TrainingSessionTest`).
- **Entering Training from the room** uses `fMainMenu::RequestTraining`, the
  same request the Home row makes, without the `StartOffline` command that row
  rides on: that command ends the room.

## In Training, as the room shows it

A member in a Training battle is shown to the others as "In Training", where
a quiet member is otherwise shown as idle for so many minutes.

- `Member::training`, a boolean. On the wire it is `"training": true`, written
  only when set and read as off when absent, as `idle_s` is written and
  `spectator_locked` is read. A state or snapshot without it reads as before.
- `ActionKind::SetTraining`, appended after `PermitReady`, carrying the wanted
  value in `Action::locked` as `LockSpectating` does. `ProtocolVersion` is
  unchanged, on that precedent: an older authority refuses the kind as
  unknown, and a room only admits members of its own build in any case.
- `RoomAuthority::ApplySetTraining`: any member may set their own flag; it
  touches the room revision when it changes and binds nothing. A fighter
  whose game starts loses it in `NormalizeMemberStatus`.
- It is the first member flag a client sets that is not about a table, so it
  is dispatched with `Rename` and `Chat`, before the table lookup.
- The runtime says it (`SayTraining`, beside `CallOutOfTraining`) by comparing
  what the room's snapshot says of the player with whether a Training battle
  is running, at most every two seconds, so a word that was lost or refused
  is said again. It goes straight to `SendRoomAction`.
- The client keeps a refusal of this action off the player's error line and
  does not let its acceptance clear an error another action left.
- `RoomTrainingFlagTest` covers the authority and the wire; the whole suite
  ran with it.

Not built with it: the room server (`server/roomhost`) compiles the same
sources and needs rebuilding to accept the action. Until then a public room's
authority refuses it, quietly, and its members show as idle as before.

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
  to end in the game. Nor have go now, the room line in Training, and the
  "Calling {name} back" line on the opponent's table.
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
3. **The Training flag on the wire.** It is added without a new
   `ProtocolVersion`, like `LockSpectating`. If a version step is wanted for a
   new action kind, it is one constant; the commit stands apart so it can be
   left out, and the call works without it.
4. **The banner** is drawn by Ember. The game's own is `ui\intrusion`, shown
   by its Arcade fight-request classes, for which there are no bindings.
