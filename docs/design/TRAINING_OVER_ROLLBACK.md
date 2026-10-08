# Training with another player over rollback

Two players in a room want to practise together: one tries a setup, the other
holds the pad, neither loses a round, and either can put both fighters back
where the drill starts. This note says what exists, what the first step built,
what the second step built, and what the third would change. Step 3 is a
proposal: nothing of it is built.

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

## Step 3, proposed: a shared reset and a shared saved position

Either player presses a button and both fighters are back at the start of the
drill, on both machines, on the same frame.

### Why it is not the offline restore

Offline, a restore loads a savestate the moment the key is seen. Over rollback
the two machines see the key on different frames, and a state loaded outside
GGPO's own timeline is the fault ledger entry A-001 ends a match for.

### How

- **The press is an input.** GGPO already delivers each side's input for a
  frame to both sides, confirmed, in order. `fPadSystem::Inputs` is two
  32-bit words; a spare bit of one carries "reset" and another "save
  position". The overlay sets the bit in the local input before
  `ggpo_add_local_input`; nothing else leaves the overlay.
- **The effect is part of the frame.** At one fixed point of every simulated
  frame, in `PlayGgpoFrame` and in the rollback callback alike, a frame
  whose synchronized inputs carry the bit applies it. A mispredicted press is
  rolled back like any other input.
- **Reset without a savestate.** Loading a savestate inside a frame is the
  dangerous part, so the first version avoids it: the reset writes the two
  positions and the facing (health and gauges fill by themselves under the
  Training rule) and puts both fighters in their standing state, through the game's own round-start path if one can be
  called, else through setters. The saved position is then six numbers kept
  in a small block that is itself saved and restored with the frame, so a
  rollback across a "save position" press brings back the earlier one.
- **Reset with a savestate, later.** A full restore (projectiles, a fighter
  in mid-air) means a state both sides hold for a frame both sides agree on.
  That is a third owner of savestates beside GGPO's pool and the offline
  checkpoint, with its own slot, taken at a confirmed frame and loaded as
  the content of a frame. It is the larger half of this step and wants the
  savestate-free work (`SAVESTATE_FREE.md`) settled first.
- **Who may press.** Both seated players. A spectator's client never sets
  the bits. A cooldown counted in frames, in the synced block, stops a held
  button from resetting every frame.

### Risks

- The spare input bits must be proven unused by the game: a bit the engine
  reads is a move input.
- The round-start path may touch state the memento does not cover: a stress
  run with the state hash compared (`SF4E_ROLLBACK_STRESS`) before any match.
- A reset during a cinematic or a throw: refused, by a test on both
  fighters' states that is itself part of the frame, until the savestate
  version exists.

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
5. **Step 3.** Whether a shared reset is wanted at all, given what it adds
   to the frame.
