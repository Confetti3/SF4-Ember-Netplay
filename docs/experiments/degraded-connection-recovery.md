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
difference over about 15 frames, and ignores anything under 0.75 frame. The
player who is ahead lengthens frames and the player who is behind shortens
them, each at half gain, so together they repay the excess over roughly one
second in steps of at most 3 ms per frame. Outstanding correction is capped at
two steps either way, so nothing stale can build up while shifts are blocked by
a prediction stall or a closed gate. The GGPO event is still counted but no
longer adds debt. The rift is sampled in every mode, including the baseline.

### How a correction reaches the frame rate

Up to v0.9.5 the correction was a wait in the outer tick, before the engine's
own FIXED frame limiter. That limiter (`0x770DE0`, on the D3D singleton, run
right after Present) spins until one period after its previous exit, so the
wait only ate into its spin and the frame rate never changed. The first real
logs (9/21, five matches) show it: 2.4 to 4.3 s of waits per match, about 3% of
match time and far more than clock drift needs, while GGPO still recommended 3
to 7 frames in 25 s bursts. The rift harness could not show this, because its
own deadline moved with the wait.

Once per outer tick, `fSystem::StepPacing` collects what the limiter applied,
samples the rift and requests the next signed shift. `fD3D::LimitFrame` then
changes the limiter's period (`+0x1F8`) for that one call. Lengthening is
exact. Shortening is limited to the time left before the deadline, less
0.25 ms, so the frame never overruns into the engine's lag catch-up (`+0x1FC`,
frame skipping). The applied amount is measured from the limiter's recorded
exits and handed back through an atomic, since the limiter may run on the
rendering thread; the controller's debt moves by exactly that amount. Every
pacer reset also drops any shift still in flight. If the limiter has no period
(possible with `FrameRate=SMOOTH`, not checked), nothing is shifted and the
debt just waits.

VSync is forced off. With it on, Present waits for a vblank, so a shift of a
few milliseconds costs a whole refresh and the controller overcorrects.
`fD3D::BuildPresentParameters` (`0x7719D0`, used for device creation and every
Reset) sets `D3DPRESENT_INTERVAL_IMMEDIATE` and logs `Display: VSync forced
off` when the game asked for anything else. The player's config.ini is not
changed, and the launch card no longer mentions VSync.

`SF4E_PACING_TEST_SHIFT_MS` (development only) makes the limiter shift every
frame by that amount instead of following netplay pacing, and logs the
measured frame every 600 frames. Offline check on 2026-09-22 at the title
screen: 0 ms gave 16.667 ms per frame, +2 ms gave 18.667 ms (1200 ms applied
per 600 frames) and -2 ms gave 14.667 ms (-1199 ms applied).

### Logs

Every 15 s of a GGPO session, and at its end, one `Netplay [...]` line gives
ping range, smoothed and maximum rift, the raw frames-behind pair, rollbacks,
re-simulated frames, deepest rollback, prediction-stall ticks, milliseconds
slowed and sped up, and GGPO time-sync events. It needs no switch.
`Pacing [...]` summaries and `FreezeCandidate` lines still report the totals.

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
Since rc2 a stall also drops any correction still owed from before it (up to
6 ms), so nothing is repaid during the hold. Coarse mode keeps GGPO's
recommendation through a stall. Gain, dead zone, smoothing and hold-off are controller fields, not yet
environment variables. The per-side gain is 1/120: at 1/60 per side the two
corrections add up and overshoot under the 200-ms delay (policy test
`rift both sides converge`).

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
project. It compiles the pacing regression source alongside 13 policy cases
(7 rift and limiter, 6 repair). Checks remain active in Release builds.

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

The harness moves its frame deadline the way `fD3D::LimitFrame` moves the
limiter. Before the limiter fix it already stretched its deadline by each wait,
so it never showed the in-game problem. Two-sided correction against the
one-sided v0.9.5 controller (2026-09-22, mean absolute rift in frames, both
peers; stalls, rollback frames and imbalance were the same in every case):

| Case | v0.9.5 | Two-sided |
| --- | --- | --- |
| Clean, 3 runs | 1.48 | 1.47 |
| 50 ms delay, 20 ms jitter, 3 runs | 1.19 | 1.14 |
| Plus 150 ms bursts every 2 s at 60.2 Hz, 6 runs | 0.97 | 1.13 |

Bursts recover slightly slower at half gain per side. Dropping pre-stall debt
(rc2) brought the burst case from 1.13 to 1.05 over 6 paired runs, with the
same stalls and rollback frames; clean and jitter runs have no stalls and did
not change. Against a v0.9.5 peer,
whose waits do nothing in the game, two-sided correction is what lets the
new side close a rift in either direction.

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
