# Training with another player over rollback

Two players in a room want to practise together: one tries a setup, the other
holds the pad, neither loses a round, and either can put both fighters back
where the drill starts. This note says what exists, what the first step built,
and what the second and third built. None of it has been played between
two machines yet.

## What exists

- The training lab is offline. `training::BeforeUpdate` takes a battle only
  when it is not network-owned and the game mode is Training. Everything the
  lab writes (position restore, dummy input, replayed combos, gauge options)
  is written on one machine.
- A match over rollback is a Versus battle. Its rules are the lobby's:
  `LobbySettings` allows a round time of 9999 and long round counts, which is
  the sparring room of today.
- The protocol still carries `trainingMode` (`LobbyData`, `LobbySetSettings`,
  `NetplayConfig`). It is vestigial: the server refuses a request that sets it
  and the config comment says the long round values replaced it. Step 2
  uses the `LobbyData` field again.

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
- The calls sit beside `CaptureHashCheckpoint`: after a played frame in
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

## Step 2, built: the Training rule of a table

A table rule, **Training**, beside the round count and the round time. With
it on, both fighters' health, recoverable health, Super and Ultra gauges fill
again while they are left alone, and nobody is knocked out.

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
- `fVsBattle::bNextMatchTraining`, set where the match's seed is taken
  (`UserApp`, after `StartGGPO` or `StartSpectating`) and applied where the
  seed is applied (`PrepareBattleRequest`): health, recoverable health, Super
  and Ultra to 8 for both players. Stun is left as in a match.
- `room::Rules::training`. On the wire `"training": true`, written only when
  set and read as off when absent, as the member flag is.
- It reaches the match the way the round count does: the table's rules into
  `LobbyData`. That struct already had a `trainingMode` field, left over from
  an earlier training room and always sent as false; it now carries the rule.
- A row in the rules list, so it is set and shown where the round time is.

### Why this is safe under rollback

Nothing is written during a frame. The values are chosen before the battle
exists and never change in it; the game's own update does the filling, on
state its own memento already saves (health and gauges are what a rollback
restores today). The two sides and every spectator take the rule from the
same table rules, at the same point they take the round count from.

A room admits only members whose sidecar has the host's own hash
(`JR_HASH_INVALID`), so no member of a room lacks the rule. The room protocol
version is unchanged.

### What it does not do

- **Reset.** That is step 3.
- **End.** Nobody is knocked out, so a game ends when the round time runs out
  or a fighter leaves. The result of a timed-out game is reported and scored
  as any game is. The rule's text tells the host to set a long round time.
- **Public rooms.** The room server is built from the same sources and needs
  rebuilding to keep the rule; until then it drops the unknown field and the
  table plays as a match.
- The old lobby path still refuses `trainingMode` in `LobbySetSettings`.

### What was checked

- `RoomTrainingFlagTest`: only the host sets it, every member's view carries
  it, it is written only when set, older rules read as before, a value that
  is no boolean is refused, and the table still starts.
- The whole suite and the UI render in every language.
- Not checked: a battle. The parameter values were read from the program,
  not seen in play. The first thing to look at with two players is the state
  hash staying equal with the rule on. A developer checkbox ("Training rules
  on next battle?") applies the rule to an offline Versus battle, where the
  rollback stress harness can be run over it.

## Step 3, built: a shared reset and a shared saved position

At a table under the Training rule either player can save where both
fighters stand and put both back there, on both machines and for every
spectator, on the same frame.

### What the game does itself

Training mode's own restart is a savestate load: `System` vtable +0x1E8
(0x5D8020) restores every unit from an internal memento, and +0x1E4 records
one. That is the mechanism `SaveState::Save` and `SaveState::Load` already
wrap for GGPO, and `Load` already allows for a state that is not GGPO's (a
state with no `ggpoFrame` leaves the result timeline alone).

### How

- **The press is an input.** Two bits of the pad's raw word,
  `PracticeReset` (0x20000000) and `PracticeSave` (0x40000000). The overlay
  asks (`training::RequestMatchPractice`); the bits go into the local input
  where it is handed to `ggpo_add_local_input` (`WithMatchPractice`), and
  are taken out of both players' inputs before the game is shown them
  (`BeforeMatchFrame`). A match without the rule passes its inputs through
  untouched.
- **The effect is part of the frame.** `BeforeMatchFrame` runs after
  `ggpo_synchronize_input` and before the engine update, in `PlayGgpoFrame`
  and in the rollback callback alike. A save takes a `SaveState` of the
  state the frame starts from; a reset loads it, and the frame is then
  played from there.
- **A press counts as it goes down** (`DecidePractice`,
  `MatchPractice.hxx`), against which presses were down the frame before.
  That small state (`PracticeState`) is saved and restored with every
  frame: it is a field of `SaveState::GlobalData`, written in `Save` and
  put back in `CopyIntoPlace`. A resimulated frame so decides as it first
  did.
- **No press is ever a prediction's.** GGPO predicts by repeating the last
  input it received, and a repeated input has no new press in it. So a save
  or a reset is only ever done on an input that was received, and the saved
  position is always the state of the real timeline: a rollback that goes
  back before the save does it again from the corrected state, and one that
  does not go back that far leaves a state that was already right.
- **The round's start is saved by itself**, on the first frame the battle
  flow is `BF__FIGHT` (the flow is in `GlobalData` too), so a reset before
  any save goes back to the start, as Training mode's does.
- **The engine's frame count is kept across a reset.** The peers' legacy
  state snapshots are compared by that count and end the match on a
  difference; counting back would set one timeline's snapshot against the
  other's.
- **The position** is the training runtime's `checkpoint`, the slot the
  offline lab keeps its own in; a match and an offline battle never run at
  once. It is released when the battle closes. Sounds that had ended when
  it was saved are marked so, as offline (`ForgetFinishedSounds`).
- **Who.** Both fighters. A spectator has no input and their overlay never
  asks. Both players are told who did what ("P1 saved the position").

### Keys

The keys the player chose in Training for Reset position and Save position
(F2 and F11 unless changed), and the pad's Select: a tap resets, held half a
second it saves. A line under the frame meter names the keys.

### What was checked

- `TrainingSessionTest`: the decision. Nothing before the fight, the start
  saved, a press counted once, whose it was, a save before a reset, other
  pad bits ignored, a predicted repeat making no press, and a rolled-back
  frame deciding as before.
- The whole suite and the UI render in every language.

### What was not

- **A match.** None of this has run between two machines. What to look at
  first: the state hash staying equal across a reset (`SF4E_STRICT_DESYNC=1`
  ends the match on a difference), a reset while a rollback is in flight,
  a reset during a throw or an Ultra's cinematic, a spectator following a
  reset, and the pad's Select reaching the overlay during a match at all.
- The legacy round-trip release (`SF4E_LEGACY_SAVESTATE_FREE=1`) with a
  kept position.

### Risks

- A load inside a frame is the one thing here that the netplay code did not
  do before. If the saved state does not come back whole the match ends, by
  the same notice as a failed rollback load.
- The two bits must not be bits the game reads. They are stripped before
  the game sees an input, and only in a match under the rule.
- The game's own replay recorder is rewound to the saved state by a reset
  (`replay::Restore`); its file for such a match is not a record of it.

## What stays offline

Dummy recordings, the dummy's replies, replayed combos, trials and the combo
creator. They drive one fighter from a local script, which over rollback is
an input source the other side cannot predict or verify. A drill partner is
the other player.

## For a decision

1. **A frame meter in real matches.** Step 1 shows frame data during play,
   to one player, about both. It gives no information the game hides, but it
   is a training aid in a match. It can be kept to spectators, or to tables
   under the Training rule; each is one condition where the
   setting is read.
2. **Results at a training table.** A timed-out game is scored like any
   other. A table under the rule could report no result and count in no set;
   that is a change to the authority and was left out.
3. **Stun** is left as in a match. Training mode offers it; it is one more
   parameter in the same call.
4. **The old `trainingMode` field** now carries the rule. If it should stay
   dead, the rule needs a field of its own in `LobbyData`.
5. **The shared reset.** It adds a savestate load to a frame of a match
   under the rule, and a field to the saved state. If that is not wanted,
   the commit stands apart and the rule works without it.
