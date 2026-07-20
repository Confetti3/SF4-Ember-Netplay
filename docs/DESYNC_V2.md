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

The GGPO save callback's frame argument is now stored per `SaveState`
(`ggpoFrame`) together with the engine frame (`simulationFrame`), hash
validity, and the semantic hashes. Slot reuse (`Clear`) resets all of it.

## Exchange policy (conservative initial approach)

- A checkpoint is captured every 30 simulated frames into a fixed 64-entry
  ring (`fSystem::hashCheckpoints`); rollback resimulation overwrites the
  entry for a re-simulated frame.
- A checkpoint is exchanged only once the sender has simulated 30+ frames
  past it — older than GGPO's 8-frame prediction window plus margin. These
  are labeled **aged / non-speculative**, NOT "confirmed": no formal GGPO
  confirmed-frame value is exposed by the current fork API. (The stronger
  approach — patching the fork to expose the last confirmed frame — is
  documented as future work and must be a separate vcpkg patch.)
- Received hashes are buffered (bounded, 64 entries) and compared only when
  the *local* checkpoint is aged too.
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
