# Training with another player over rollback

Two players in a room want to practise together: one tries a setup, the other
holds the pad, neither loses a round, and either can put both fighters back
where the drill starts. This note says what exists, what the first step built,
and what the next two steps would change. Steps 2 and 3 are proposals: nothing
of them is built.

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
  and the config comment says the long round values replaced it. It is not
  reused below; see "For a decision".

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

## Step 2, proposed: training rules for a table

A table rule, **Training**, that both sides apply to the same battle: health
that comes back, gauges that refill, no time over.

### What has to be true

The rule changes the simulation, so it is part of the deterministic state:
both clients and every spectator must apply the same writes on the same
frames, and a rollback must replay them.

### How

- **The rule** travels as the round count and the timer do: a field of the
  table's rules in the room model, shown to both players before they ready,
  fixed at the Preparing table and carried in the match's start parameters.
  A client that does not know it must not be seated at such a table.
- **Applying it** is a pure function of the game state, run at one fixed
  point of every simulated frame, in `PlayGgpoFrame` and in the rollback
  callback alike (the same two places step 1 uses, before the update instead
  of after). No clock, no local setting, no input from the overlay.
- **Health and gauges.** The game already has the behaviour: in Training its
  hit code and gauge code read the 14 options of `Training::Manager` every
  frame (gauges: 0 normal, 5 max, 7 infinite, 8 refill). Two ways to get it
  in a Versus battle over rollback:
  1. *Run the battle as Training.* The game mode goes into the battle request
     on both sides and the options are set from the rule. Least code of ours
     in the frame, but Training mode brings its own pause menu, recorder and
     dummy driver, and none of that has been run under rollback. The pause
     menu alone would have to be closed off.
  2. *Stay Versus and write the values.* Each frame, when a fighter is in a
     neutral state and no combo is counted, set health and gauges to the
     rule's values through the actor. Needs setters that are not bound yet
     (only the getters are), and each needs its place in the memento checked
     so a rollback restores what was written.

  The second is the safer one to review: the battle stays the one netplay
  already runs, and the added writes are few and can be covered by the
  rollback stress harness (`SF4E_ROLLBACK_STRESS`), which replays frames
  offline and compares.
- **Round end.** With health restored on neutral, a round ends only by a
  combo that kills. Either that stands (a kill ends the round, the long round
  count absorbs it), or the rule also stops the last hit from killing. The
  first needs nothing.
- **Results.** A training table reports no result and counts in no set: the
  rule switches `PublishConfirmedNativeMatchResult` and the set score off for
  that match. Rotation at such a table needs a rule of its own: by time, or
  by a player leaving the seat.

### Risks

- Anything written per frame outside the memento desyncs on the first
  rollback. Each write needs a stress run with the state hash compared.
- The state hash (`CaptureHashCheckpoint`) will catch a client that applies
  the rule differently, as a desync. That is the wanted failure, not a silent
  one.
- A rule one of two builds does not know splits the table. How the room
  keeps such a client out of the seat has to be settled with the rule; the
  `SetTraining` action went in without a protocol version step, a rule that
  changes the simulation cannot.

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
- **The effect is part of the frame.** At the fixed point of step 2, a frame
  whose synchronized inputs carry the bit applies it. A mispredicted press is
  rolled back like any other input.
- **Reset without a savestate.** Loading a savestate inside a frame is the
  dangerous part, so the first version avoids it: the reset writes the two
  positions, the facing, health and gauges, and puts both fighters in their
  standing state, through the game's own round-start path if one can be
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
- The round-start path may touch state the memento does not cover. Same
  check as step 2: stress run, hash compared.
- A reset during a cinematic or a throw: refused, by the same neutral-state
  test as the health write, until the savestate version exists.

## What stays offline

Dummy recordings, the dummy's replies, replayed combos, trials and the combo
creator. They drive one fighter from a local script, which over rollback is
an input source the other side cannot predict or verify. A drill partner is
the other player.

## For a decision

1. **A frame meter in real matches.** Step 1 shows frame data during play,
   to one player, about both. It gives no information the game hides, but it
   is a training aid in a match. It can be kept to spectators, or to tables
   under the step 2 rule once that exists; each is one condition where the
   setting is read.
2. **Which way for step 2:** the battle run as Training, or Versus with
   written values. This note recommends the second.
3. **The old `trainingMode` field.** Step 2 could revive it or add a table
   rule beside the round count. A table rule fits the room model as it is
   now; the old field belongs to the lobby protocol the rooms replaced.
4. **Results at a training table:** none at all, as proposed, or a set that
   is played and not counted.
5. **Order.** Step 2 is useful alone (spar without rounds ending). Step 3's
   first version depends on step 2's fixed point and its setters.
