# Degraded-connection recovery experiment

Base: v0.9.3 / `805e176993d35e7ccc4c586441edd3c6249c813c`.
Branch: `experiment/degraded-connection-recovery`.

This is experimental source for pre-release builds. It is measured in the rift
harness but not yet verified in gameplay. Both experiments are **on by default**
because that was the best configuration in every harness case; set a switch to
`0` to turn it off. Neither changes the wire format, so a pre-release player can
play someone on v0.9.3; each side simply gets the benefit of what it runs.
Setting these variables on the unmodified v0.9.3 download does nothing.

## Experiments and ownership

`SF4E_CONTINUOUS_TIMESYNC` (on unless `0`) replaces the coarse GGPO time-sync event with a
per-tick correction in the existing pacing controller. Stock GGPO only acts on
a rift of 3 frames or more, at most once every 240 frames, as a lump of up to 9
frames. A smaller rift is never corrected, so one player carries most of the
rollbacks. With the switch on, the outer tick reads `local_frames_behind` and
`remote_frames_behind` from `ggpo_get_network_stats`, smooths half their
difference over about 15 frames, ignores anything under 0.75 frame, and repays
the excess over roughly one second through the same 3-ms-capped waits. Only the
player who is ahead waits. Outstanding correction is capped at two slices, so
nothing stale can build up while waits are blocked by a prediction stall or a
closed gate. The GGPO event is still counted in diagnostics but no longer adds
debt. `Pacing [...]` summaries and `FreezeCandidate` lines report the rift.

`fast-quality-report.patch` lowers GGPO's quality report interval from 1000 ms
to 100 ms so the remote half of that measurement is fresh. It is wire
compatible, costs about ten small packets per second, and applies with both
switches off, so the baseline on this branch differs from v0.9.3 in that one
respect (ping and the coarse estimate also update faster).

The rift is measured in whole frames by GGPO and the peer's half arrives about
100 ms plus one trip late. The slow gain is what keeps that delay from causing
overshoot; the policy test closes the loop with a 200-ms delay. Samples are
dropped during a prediction stall and for 45 ticks after it: measured in the
rift harness, the estimate is within about one frame when calm but off by up to
seven frames right after a stall, because the last received frame is stale.
Gain, dead zone, smoothing and hold-off are controller fields, not yet
environment variables.

`SF4E_GGPO_INPUT_REPAIR` (on unless `0`) enables a quiet-input repair policy inside each GGPO
endpoint. It regenerates the current pending input history with a fresh packet
sequence and current ACK; it does not replay an old Iroh datagram. Ordinary
input sends suppress extra repairs. Repairs start after a 33-ms quiet period,
back off through 66/132/200 ms, and add at most four packets in any rolling
one-second window. Their byte bound is four times the existing admitted packet
size, plus transport overhead. ACK progress can reset the backoff but cannot
refill the rolling packet budget. Duplicate/impossible ACKs do neither.

The shipping 200-ms fallback remains first in the poll path; repair cannot add
a second send in that poll. The experiment excludes spectator send endpoints,
terminal endpoints, GGPO's pending send queue, and its artificial reordering
slot. The normal framing, pending-output limit, ACK processing, disconnects,
and prediction window are unchanged. Fully ACKed input is not retransmitted
just because GGPO intentionally retains its last ACKed frame.

The policy observes send silence and unacknowledged input, not an explicit
simulation-stall event. Slow local processing can also activate it. It cannot
see Iroh's congestion window or underlying relay queues. The rate cap bounds
extra traffic but does not prove that extra traffic helps a congested link.
There is no new ACK-only response protocol, public repair counter, or HUD here.

## Four-way comparison

Restart the launcher/game for each configuration. Set variables in the shell
that starts the test launcher so the game inherits them.

| Configuration | SF4E_CONTINUOUS_TIMESYNC | SF4E_GGPO_INPUT_REPAIR |
| --- | --- | --- |
| Baseline | 0 | 0 |
| Time sync only | 1 | 0 |
| Repair only | 0 | 1 |
| Both | 1 | 1 |

Unset means `1`. For the baseline, from the directory containing the **newly
built** Launcher.exe:

```powershell
$env:SF4E_CONTINUOUS_TIMESYNC = '0'
$env:SF4E_GGPO_INPUT_REPAIR = '0'
.\Launcher.exe
```

Each match logs `Netplay experiments: continuousTimesync=... inputRepair=...`.

Use exactly `0` or `1`. The switches use the existing environment mechanism;
no persistent player preference, wire-format change, or second controller was
added. Keep input delay, stage, hardware settings and opponents constant.

## Automated checks

The standalone test build avoids the Windows/game dependencies of the main
project. It compiles the existing pacing regression source unchanged alongside
11 policy cases (5 rift, 6 repair). Checks remain active in Release builds.

```sh
cmake -S src/tests/degraded-connection -B build/degraded -DCMAKE_BUILD_TYPE=Release
cmake --build build/degraded --config Release
ctest --test-dir build/degraded -C Release --output-on-failure
python scripts/verify_ggpo_experiment.py
# Windows SDK/compiler required for this additional check:
python scripts/verify_ggpo_experiment.py --build
```

The verification script uses a temporary checkout of the pinned GGPO revision,
applies every patch in portfile order, copies the same policy header used by
the unit tests, and optionally compiles GGPO. It does not build Ember or run the
game. The branch-scoped GitHub Actions workflow runs portable tests on Linux
and Windows, checks the full patch stack, and builds GGPO on Windows. It does
not publish a release, sign binaries, or write back to the repository.

Local validation: both CTest targets pass with MSVC in Release mode at /W4
with warnings as errors, and the full patch stack applies to the pinned GGPO
revision. Gameplay and real network testing have not been done. Consult the
workflow result separately; adding a workflow is not evidence that it passed.

The fallback scheduling fixture demonstrates why a short loss burst might
recover earlier. It intentionally has no latency, QUIC, game state, or real ACK
wire. Its synthetic timings are **not** a measured end-to-end latency gain.

## Rift harness

`RecoveryBenchmark.exe --rift` runs two GGPO peers on their own threads and
clocks (one at `--fast-hz`, default 60.5) through the loopback impairment proxy,
with the real pacing controller. It needs no helper and no game.

```powershell
$env:SF4E_GGPO_INPUT_REPAIR = '0'   # only to turn repair off, as in the game
.\RecoveryBenchmark.exe --rift --frames=1200 --drop-every=0 `
    --delay-ms=50 --jitter-ms=20 --burst-ms=150 --burst-every-ms=2000 --fast-hz=60.2
```

Add `--coarse` for the stock GGPO time sync. Output per peer: frames,
rollback frames and depth, prediction stall ticks, mean and p95 true rift (own
frame minus the peer's), pacing wait, plus the rollback imbalance between the
peers. Bursts are wall-clock based, so runs vary; compare averages of several.
Inputs change every 8 frames and there is no game cost, so absolute rollback
numbers do not transfer to SF4. Only the comparison between modes does.

## Gameplay acceptance gate

Run the four configurations under random loss (1/5/10%), bursts (50/100/200 ms),
one-direction-only loss, reordering, variable delay, saturated upload, and CPU
hitches. Repeat direct and forced-relay cases, with and without spectators.
Test both application-level drops and actual path impairment; these exercise
different layers. Verify rematch, result confirmation, disconnect/reconnect,
and long-running sessions as well as the first round.

Record prediction-stall counts/durations, rollback depth and save/load cost,
outer-frame timings, time-sync correction and rift, input delay, bridge drops
and send pressure, and the route. Capture network traffic to verify repair
cadence and actual packet delivery; the current UI does not expose all of the
needed input-progress telemetry. Require identical confirmed-state outcomes,
no clean-link regression, bounded queues/traffic, and no increase in spectator
impact. Do not promote the switches solely because the policy tests pass.

## Scope

The repair policy lives in GGPO, not in the Iroh bridge, and its single header
is shared by production and tests. Rift correction lives in the existing pacing
controller and uses its lifecycle reset. There is no background packet queue,
second pacing loop, mid-round input delay change, FEC layer or new
acknowledgment protocol.

Remaining blocker for promotion is measurement: the two-peer rift harness and a
two-PC session under real impairment.
