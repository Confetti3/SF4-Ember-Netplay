# Desync detection v2 (semantic hash checkpoints)

Status: **experimental / validation phase.** The legacy 60-frame
`StateSnapshot` exchange stays fully operational (including its existing
mismatch termination). v2 adds classification and earlier, cheaper
checkpoints; it does not replace v1 until it has demonstrated reliability.

## What is hashed

`fSystem::ComputeSemanticHashes` (canonical encoder: `sf4e__StateHash.hxx`,
64-bit FNV-1a over explicit little-endian field-by-field bytes):

- **flow subsystem hash**: engine frames-simulated (fractional+integral),
  Current/Previous battle flow and substate (u32 each), the four battle-flow
  frame FixedPoints.
- **per-character subsystem hashes** (2): status, side, root position
  (4 float bit patterns), vitality/max, revenge/max, recoverable/max,
  super/max, SC time/max, UC time/max, combo damage, damage — all read via
  engine getters, the same values the legacy snapshot already proves
  comparable across peers.
- **action timing** (v0.8.6, inside each per-character hash): action id,
  action frame (fixed point), action posture, and the side's unit time scale
  (hitstop and slowdown). These are the Training Lab frame-meter getters. A
  replay that keeps positions and health but reaches a move or its active
  frames on a different frame now shows up as a character mismatch. Hitbox
  geometry and the evolving RNG are still not hashed.
- **overall** = hash of the three subsystem hashes.

## Explicitly excluded (do not add without a determinism argument)

- Battle-flow function pointers (`BattleFlowSubstateCallable_aa9258`,
  `BattleFlowCallback_CallEveryFrame_aa9254`) — process-local addresses.
- The raw `GameManager` block — shallow-copied pointer fields.
- `GameMementoKey` object bytes — mix stable payload with process-local
  ownership metadata.
- Sound maps — pointer-keyed by process-local adapter addresses.
- RNG: the *evolving* RNG state has not been located; the initial match
  seed is not it and hashing it would prove nothing.
- Any raw struct memory (padding, uninitialized bytes), wall-clock values,
  log/overlay/presentation state.

## Frame identity

The GGPO save callback's frame argument is stored per `SaveState`
(`ggpoFrame`) together with the engine frame (`simulationFrame`). Slot reuse
(`Clear`) resets both. Save states hold no hashes; the semantic hashes live
only in the checkpoint ring below. A training or stress load carries no
GGPO frame (`ggpoFrame == -1`) and does not rewind the native-result timeline.

## Exchange policy

- A checkpoint is captured every 30 simulated frames into a fixed 64-entry
  ring (`fSystem::hashCheckpoints`); rollback resimulation overwrites the
  entry for a re-simulated frame.
- A checkpoint is exchanged only once GGPO has confirmed every input that
  contributed to it. The pinned fork exposes the last confirmed input frame
  through `ggpo_get_last_confirmed_frame`
  (`vcpkg-overlays/ports/ggpo/confirmed-frame-accessor.patch`); `IsConfirmedCheckpoint`
  in `ConfirmedCheckpoint.hxx` is the gate. Spectators have no save callback
  and use the confirmed boundary plus one as their state frame.
- Received hashes are buffered (bounded, 64 entries) and compared only when
  the *local* checkpoint is confirmed too.
- Only players send (`fromPlayer=true`); spectators receive forwarded
  hashes and compare locally as diagnostics.
- Storage is bounded everywhere; hashes are computed from ~50 getters per
  checkpoint (every 30 frames) — no per-frame JSON.

## Mismatch policy

On mismatch v2 logs the exact frame, per-subsystem match/mismatch
classification, and all nearby checkpoints (±90 frames), and leaves the
legacy snapshot machinery untouched. It does **not** terminate the match by
default. `SF4E_STRICT_DESYNC=1` enables strict debug termination, and only
when *both* peers are players — a spectator mismatch can never end the two
players' fight.

## Compatibility

The existing `sidecarHash` join gate guarantees both clients run the same
build and therefore the same encoder and protocol — this is the documented
compatibility mechanism for v2; no separate capability negotiation is
added. `battle_hash` uses `WITH_DEFAULT` deserialization for
forward-compatible field additions. Servers that predate the message do not
forward it (clients then silently fall back to v1-only verification);
updated servers forward it exactly like `battle_snapshot`.

## Local rollback stress (one PC)

`SF4E_ROLLBACK_STRESS=<1..8>` makes an offline Versus or Training battle drive
save states the way a GGPO session with that rollback distance does:

- every frame frees the oldest of ten ring slots and saves before simulating;
- every `<distance>` frames the state from `<distance>` frames ago is loaded
  and those frames are re-simulated with their recorded inputs, freeing and
  saving on each one.

After each re-simulated frame the semantic hash is compared with the original
pass. A difference logs `RollbackStress: replay diverged` with the frame, the
free path, which subsystem differed (flow, p1, p2) and the inputs. Training Lab
playback drives the recorded side, so a specific sequence can be replayed
under rollback, for example the reported C. Viper crouching medium kick into
a special and then Super against Dudley. Recording is interrupted and the history restarts when the
simulation does not advance by exactly one frame (pause, training restore).

Combine it with `SF4E_ROLLBACK_DIAGNOSTICS=1` for the timing summary every 600
frames, and with `SF4E_LEGACY_SAVESTATE_FREE=1` to compare the two free paths
([SAVESTATE_FREE.md](SAVESTATE_FREE.md)). GGPO's synctest backend is not used
because it breaks into the debugger on a mismatch and writes a log file per
frame.

This checks that one machine replays its own frames identically. It does not
replace two-PC validation: peers can still diverge through state that the
hash does not cover.

## Live validation checklist (not automatable offline)

- Two-player match, `SF4E_ROLLBACK_DIAGNOSTICS=1`: no v2 mismatch across a
  full set (rollback-heavy inputs included), including rematches.
- Same with a spectator attached: spectator sees no mismatch; a forced
  spectator-side desync (e.g. spectator-only build difference) must not end
  the players' match.
- Local determinism: save/load/resimulate to the same frame reproduces the
  same hash (observable by comparing re-captured checkpoint values after
  rollbacks — any self-mismatch would surface as spurious peer mismatches
  at aged checkpoints).
- `SF4E_STRICT_DESYNC=1` on both players: an artificially injected
  divergence terminates within ~60 frames of the divergent checkpoint.
