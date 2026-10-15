# Training with another player over rollback

Two players in a room want to practise together: one tries a setup, the other
holds the pad, neither loses a round, and either can put both fighters back
where the drill starts. The frame meter and Practice rooms are kept;
shared position save/reset is retired (step 3).
The implementation checks below do not establish two-machine gameplay acceptance.

## What exists

- The training lab is offline. `training::BeforeUpdate` takes a battle only
  when it is not network-owned and the game mode is Training. Everything the
  lab writes (position restore, dummy input, gauge options)
  is written on one machine.
- A match over rollback is a Versus battle. Its rules are the lobby's:
  `LobbySettings` allows a round time of 9999 and long round counts, which is
  the sparring room of today.
- The protocol carried `trainingMode` in `LobbyData`, `LobbySetSettings` and
  `NetplayConfig`, all vestigial. Step 2 uses the `LobbyData` field again;
  `LobbySetSettings` no longer has it, and `NetplayConfig` keeps its byte only
  for the payload's layout.

## Step 1, built: the frame meter in a match

A setting, **Frame meter in matches**, off by default. With it on, the
training frame meter is drawn over the player's own matches and over matches
they watch. Only that player sees it.

- It only reads. `training::ObserveMatch` calls the same getters the offline
  meter calls after an update (`ReadFighters`), on the game thread, and writes
  nothing to the game. With the setting off it returns before reading.
- It never shows a predicted frame. Every simulated frame is captured under
  its GGPO save frame, a resimulated one over its first capture
  (`training::ConfirmedSamples`, built as `native_result::Timeline` is). A
  frame goes to the meter once `statehash::IsConfirmedCheckpoint` says every
  input in it is confirmed. The meter therefore trails the fight by the
  frames still predicted, and what it showed is never taken back.
- The calls sit beside `CaptureSnapshot`: after a played frame in
  `PlayGgpoFrame`, where the confirmed boundary is asked for, and after a
  resimulated frame in `ggpo_advance_frame_callback`, where the frame is only
  captured.
- A spectator plays confirmed inputs only and has no save frame, so their
  frames go straight to the meter.
- Not read in a match: the fighter's script file. A projectile move whose
  header names no attack frames shows no startup there. The read is file I/O
  and was kept out of the rollback frame.
- `TrainingSessionTest` covers the capture and release order, a replayed
  frame, a hole, the slot wrap and a new match.

Whether a meter belongs in a match at all is a decision; see below.

## Step 2, built: Practice rooms

A private room created with **Practice room** on plays every game as a
Practice match: both fighters' health, recoverable health, Super and Ultra
gauges fill again while they are left alone, and nobody is knocked out. It
began as a per-table rule beside the round count and the round time; that
rule was applied one match late and could be turned on and off between
games, so it became a property of the room, chosen on Create and fixed for
the room's life.

### How the game does it

The game already has the behaviour and Training mode is only one user of it.
A battle request holds a table of parameters a player (`Request` +0xF0, 0x2B
of them, 0x2B8 bytes a player). The fighter's own update reads five of them
every frame through `Battle::System` (vtable +0x158):

| Parameter | Gauge |
| --- | --- |
| 0x1A | health |
| 0x1C | recoverable health |
| 0x1E | Super |
| 0x21 | Ultra |
| 0x24 | stun |

and acts on the value: 0 as in a match, 6 held empty, 7 held full, 8 filled
again once the fighter has been left alone for a second. With health at 8 the
hit code also skips the knockout (0x55C89A). In Training mode the same reader
returns the pause menu's choices instead of the request's.

The request has a setter for them (`Request::SetPlayerParam`, 0x6851A0) that
refuses a value outside the parameter's range. It sits beside
`SetRandomSeed`, which netplay already calls.

### What was built

- `Request::SetPlayerParam` is bound, with the parameter and value names.
- `battle::NextRequest` (`fVsBattle::nextRequest`) holds the match's seed and
  the rule together. `StartMatchFromLobby` arms it from the frozen room
  projection when it enters the match, beside the round count and time, and
  `PrepareBattleRequest` takes it once: health, recoverable health, Super and
  Ultra to 8 for both players. Stun is left as in a match. The game prepares
  the request before the battle registers the tasks that start GGPO, so
  anything set at GGPO start reaches only the following battle; an earlier
  build did that and played the rule and the seed one match late.
- `room::Snapshot::practice`, set once when the room authority is made
  (`RoomAuthority`, `SessionServer::EnableCustomRooms`) and changed by no
  action. On the wire `"practice": true` in the snapshot and the checkpoint,
  written only when set, like `server_owned`. A public room is never one
  (`SetServerOwned` refuses it), nor is a tournament room (`BindTournament`
  refuses it). A replacement room keeps the flag of the room it replaces.
- It reaches the match the way the round count does: the room's table
  projection fills `LobbyData`. That struct already had a `trainingMode`
  field, left over from an earlier training room; it now carries the flag,
  and a Practice room projects the longest round time
  (`room::MatchRoundTime`) whatever the table's rules hold.
- A **Practice room** row on the private Create screen, off each time the
  screen opens and never saved with the room defaults. It travels on the
  `HostRoom` command into `AttachRoom`. The room's status line, its table
  rules summary and its table cards say Practice room, and its tables have
  no round time row.
- Rules and room defaults an older build wrote with the per-table rule
  (`training`, `time_before_practice`, `timeBeforePractice`) still load; the
  keys are not read.

### Why this is safe under rollback

Nothing is written during a frame. The values are chosen before the battle
exists and never change in it; the game's own update does the filling, on
state its own memento already saves (health and gauges are what a rollback
restores today). The two sides and every spectator take the flag from the
same room projection, at the same point they take the round count from.

A room admits only members whose sidecar has the host's own hash
(`JR_HASH_INVALID`), so no member of a room lacks the flag. The room protocol
version is unchanged.

### What it does not do

- **Reset.** There is no shared position save/reset (step 3 below).
- **End.** Nobody is knocked out, so a game ends when the round time runs out
  (always the longest, 9999) or a fighter ends it. The result of a timed-out
  game is reported and scored as any game is. A fighter ends it with End
  practice: Start or F10 opens Ember over the fight
  (`input::MenuContext::PracticeMatch`, from the flag as fixed when the
  battle was prepared), and the row sends the room's `AbortMatch` for the
  live generation. The room ends the game with no result and every member
  tears it down from its `MatchEnded`, with the neutral notice "Practice
  ended" in a Practice room instead of "This table's game was cancelled".
  The authority accepts `AbortMatch` from a fighter in any room: a fighter
  whose game fails reports it the same way (`ReportMatchAbort`), so only
  the client offers End practice, and only in a Practice match
  (`AbandonableAt`).
- **Public and tournament rooms.** Never Practice rooms.

### What was checked

- `RoomTrainingFlagTest`: the flag is in every member's view and the
  checkpoint, written only when set, refused when it is no boolean, and
  unchanged by rules, games or the host leaving; the old per-table key is
  not read. `RoomServerOwned` and `RoomTournament`: public and tournament
  rooms refuse it. `PracticeRoomProjection` and `SessionClientTransport`:
  a Practice room projects `trainingMode` and round time 9999.
  `NextBattleRequest`: the flag and seed reach only the battle prepared for
  the match that armed them.
- The whole suite and the UI render in every language.
- Not checked: a battle. The parameter values were read from the program,
  not seen in play. The first thing to look at with two players is the state
  hash staying equal in a Practice room. A developer checkbox ("Training
  rules on next battle?") applies the rule to an offline Versus battle,
  where the rollback stress harness can be run over it.

## Step 3, retired: shared position save/reset

A shared checkpoint was built and then disabled (`861d83a4`); it is now
retired and its code is gone. The offline lab's checkpoint is not part of the
rollback state, so it cannot save or restore a match's position.

- Nothing of it is in the rollback save state any more.
- Two bits of the pad's raw word (`training::ReservedInputBits`) carried the
  presses. In a Practice match `ClearReservedInputBits` still takes them out
  of the local input before GGPO and out of both synchronized inputs before
  the game plays a frame, as it did before, so the game never sees them.
  In any other match the inputs are left alone.
- The match HUD says shared save and reset are not available
  (`DrawMatchPracticeNotice`).
- The Practice match's refill/no-knockout rule and confirmed-frame meter
  remain separate from offline dummy and checkpoint mutations.

Offline Training keeps its own Save position and Reset position controls.

## What stays offline

Dummy recordings and the dummy's replies. They drive one fighter from a local
script, which over rollback is an input source the other side cannot predict
or verify. A drill partner is the other player.

## For a decision

1. **A frame meter in real matches.** Step 1 shows frame data during play,
   to one player, about both. It gives no information the game hides, but it
   is a training aid in a match. Decided: it stays the player's setting in
   private rooms, and a Practice room's game always shows it
   (`match_frame_meter::Wanted`, from the flag as fixed when the battle was
   prepared). A public room never shows it.
2. **Results in a Practice room.** A timed-out game is scored like any
   other. A Practice room could report no result and count in no set; that
   is a change to the authority and was left out.
3. **Stun** is left as in a match. Training mode offers it; it is one more
   parameter in the same call.
4. **The old `trainingMode` field** now carries the room's flag. If it should
   stay dead, the flag needs a field of its own in `LobbyData`.
5. **The shared reset.** Decided: retired. A Practice room works without a
   shared checkpoint.
6. **Practice per table or per room.** Decided: per room, chosen on Create
   and fixed for the room's life. To play normal matches the players close
   the room and create a new one.
