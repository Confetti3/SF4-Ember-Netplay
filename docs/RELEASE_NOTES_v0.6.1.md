# SF4 Netplay Launcher v0.6.1 (testing)

> **Experimental unofficial test build** — not production-ready software.
> Use it with people who understand that matches may still freeze, disconnect,
> or expose incomplete diagnostics.

This build preserves the rollback and frame-pacing improvements tested in
v0.6.0-rc1, while correcting telemetry and lifecycle defects discovered in
the follow-up audit. Its purpose is to classify the remaining occasional
freezes and compare distributed pacing on versus off.

## Install

1. Download `sf4-netplay-launcher-*-0.6.1.zip` from this release.
2. Extract the complete zip into a new folder.
3. Run `preflight.cmd`, then `Launcher.exe`.
4. Confirm both players show **v0.6.1**.
5. Use the same zip, transport mode, and input delay on both machines.

Mixed builds are rejected by the room compatibility check.

## What changed since v0.6.0-rc1

- A continuous GGPO prediction-threshold stall now remains one diagnostic
  episode across outer application ticks. Every refused frame is still
  counted, and the duration ends only when deterministic simulation advances
  or the session terminates.
- Removed unused semantic hashing from every normal and rollback-resimulated
  GGPO save. Semantic hashes are still computed at periodic comparison
  checkpoints and now have a dedicated timer.
- Old-session teardown is logged before new-match diagnostics are reset.
  Save-state retirement, temporary Free saves, and occupied-slot gauges are
  classified correctly.
- Added complete outer-tick hitch measurement and explicit per-operation
  counts at 16.67, 25, 33.33, 50, and 100 ms.
- Added rate-limited `FreezeCandidate` records with simulation/GGPO frames,
  gate and connection state, rollback activity, pacing debt and waits,
  Save/Load/Free timings, occupied slots, transport, ping, and frame
  advantage.
- The tested GGPO gate model now participates in production advancement.
  A RUNNING or connection-resume event cannot cancel an independent manual
  pause or terminal state.
- Added a distributed time-sync pacing A/B switch and detailed requested
  versus actual wait accounting.

## Test switches

Enable rollback diagnostics before launching both peers:

```text
SF4E_ROLLBACK_DIAGNOSTICS=1
```

Distributed pacing remains enabled by default:

```text
SF4E_GGPO_DISTRIBUTED_TIMESYNC=1
```

For the comparison run, disable only pacing:

```text
SF4E_GGPO_DISTRIBUTED_TIMESYNC=0
```

Do not change transport or input delay between the pacing-on and pacing-off
runs. See `docs/ROLLBACK_HARDENING_LIVE_AB.md` for the full two-machine matrix
and freeze collection checklist.

## Known limitations

- This build has passed automated and offline UDP tests, but the new
  diagnostics and gate integration still require live two-player validation.
- `FreezeCandidate` identifies a slow complete outer tick; it cannot by
  itself identify time spent in uninstrumented engine or driver work.
- If one original player loses only the room/control connection, the surviving
  peer may not yet mark verification unavailable.
- v2 hash messages are still keyed only by frame and trust the payload's
  player/spectator flag. Strict v2 desync termination remains disabled by
  default.
- Pacing is enabled by default because the previous live match improved with
  it. The A/B flag is diagnostic, not a recommendation to disable pacing
  permanently.

Report visible freezes with logs from both peers, approximate timestamp,
observed duration, audio behavior, transport, input delay, ping, and the
nearest `FreezeCandidate` record.
