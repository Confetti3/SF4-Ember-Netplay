# Zero delay correctness and pacing audit

Zero delay and separate player delays are supported by this GGPO integration at the input-queue level. The controlled tests below establish that those configurations can recover the correct synthetic state under delayed, reordered and lost packets. They do not establish that native USFIV always restores correctly or stays within its rendering budget.

The practical recommendation is to separate three decisions: repairing rollback correctness, allowing an optional zero setting, and choosing the default. Following this audit, the requested local policy is a default of one frame with zero still selectable. Choosing manual zero shows a warning that it remains under testing and may cause desyncs. Existing explicit saved values are preserved. This policy does not establish native acceptance or change rollback mechanics.

Audit date: 2026-10-09. Source baseline: `39b81157452c223a61f6bf7b7e4f051c99f76940`, with local test-only additions at measurement time. The default and warning update follows those measurements. GGPO is the repository's patched `adanducci/ggpo@c88b667`, not an unmodified latest upstream checkout. Exact measured executable, DLL and fixture hashes are retained in `build/delay-audit/provenance.json`.

## What the delay setting does

An input sampled at simulation frame F is scheduled for F + D, where D is that player's configured delay. Zero removes this additional input buffering; it does not remove the game's own input, display or network latency. GGPO predicts missing remote inputs using the last known input, restores an earlier state when that prediction proves wrong, and replays to the current frame. Its input queue initializes delay to zero. The public API accepts a player handle when setting local delay. See the official [input queue](https://github.com/pond3r/ggpo/blob/master/src/lib/ggpo/input_queue.cpp), [API contract](https://github.com/pond3r/ggpo/blob/master/src/include/ggponet.h) and [synchronization implementation](https://github.com/pond3r/ggpo/blob/master/src/lib/ggpo/sync.cpp).

In the pinned fork, `Sync::AddLocalInput` stamps the input, the local queue applies its delay, and `Peer2PeerBackend::AddLocalInput` sends the resulting frame number. The other peer receives an already scheduled input. Applying that remote player's delay again would double it. The current Ember startup code applies only the local seat's committed value before simulation begins; the remote queue keeps its zero offset. The two players do not need equal settings to agree on the resulting input history.

These are independent input settings, not independent experiences. If P1 chooses zero, P2 has less advance notice of P1's actions. Raising only P2's delay does not buffer P1's inputs. At equal simulation clocks, it primarily gives P1 more notice of P2's inputs. The clock controller can subsequently redistribute the prediction burden.

At 60 simulation frames per second, one frame is about 16.67 ms. A reported 40 ms RTT is a round trip, not necessarily 40 ms in each direction. Even an approximately 20 ms one-way path depends on polling phase, jitter, scheduling and clock differences. There is no universal 16 ms ping boundary where zero becomes incorrect, and one frame at 40 ms RTT does not guarantee zero corrections. A correction may be visually harmless, especially when it affects little movement.

## Native integration findings

The relevant path is `StartRuntimeGgpo` in `sf4e__UserApp.cxx`, `StartGGPO` and callbacks in `sf4e__Game__Battle__System__Ggpo.cxx`, and `BattleUpdate` in `sf4e__Game__Battle__System.cxx`.

* The committed seat delay reaches the local GGPO handle. It is not replaced with the opponent's maximum in this custom-room path.
* The forward tick polls GGPO before sampling input, synchronizes inputs, runs one original engine update, then advances GGPO. Replay uses synchronized inputs and the original engine update rather than recapturing the controller.
* The fork saves frame zero before accepting its first local input. Ember's save callback has no special rejection of zero or of a zero-delay session.
* The prediction limit is eight frames. On a prediction-threshold refusal, the forward tick does not update the engine or advance GGPO. Recovery can therefore preserve correctness while visibly stalling.
* The native save pool has ten slots, derived from the prediction limit plus two. Zero delay does not request an unbounded rollback history.
* The timeline latch keeps round-transition idle updates within GGPO once the battle timeline starts. This addresses a concrete mismatch between forward and replay frame accounting.
* Native save/load relies on reverse-engineered engine mementos, battle-flow globals, the GameManager copy, replay recorder state and sound state. Semantic hashes cover selected flow and character fields. In particular, the evolving RNG is not included in those hashes; an initial shared seed is not proof of complete RNG restoration. This is a coverage limitation, not evidence that RNG is currently broken.

The earlier paired desync logs agree at GGPO checkpoints 6240, 6270 and 6300 and disagree at 6330. Their engine-minus-GGPO frame offsets differ by three. The round-transition routing mismatch is consistent with this evidence and has a focused model regression test. The logs do not establish that the numeric delay value itself caused a process crash. They show a mismatch that the desync guard acted on.

The subsequent acceptance record reports four zero/zero starts and four confirmed results, no local desync/error reports, and a stable offset of one. That supports the repair but does not prove the original failure cannot recur. The opponent's later helper log lacks matching native semantic hashes, so it cannot close that evidence gap.

## Controlled correctness results

`GgpoDelayRollbackTest` runs two real GGPO sessions through a loopback packet proxy at a nominal 60 Hz. State contains a frame index and an input-driven rolling digest. Save and load restore both; replay consumes GGPO's corrected inputs. At the end, both peers must confirm all target inputs and match an independently generated, delay-adjusted input history. Equal peer frame numbers alone cannot pass the test.

All 27 matrix cases passed, with 300 accepted simulation frames per peer in each case:

* 0/0, 1/1 and 2/2 at added one-way delays of 0, 8, 20, 40, 80 and 150 ms, without loss. Inputs change every eight simulation frames.
* 0/1, 1/0, 0/5, 5/0, 0/10, 10/0, 0/0, 1/1 and 2/2 with 40 ms added one-way delay, 0-30 ms deterministic jitter and a scheduled drop every 17 datagrams per direction. Inputs change every frame to stress prediction. Jitter can reorder packets.

Selected results below are additional replayed frames per peer over 300 accepted frames. These measure replay work, not milliseconds of native CPU time or the number of visible animation corrections.

| Added one-way delay | Input delay | Replay frames P1 / P2 | Maximum rollback depth | Stalled ticks P1 / P2 |
| --- | --- | --- | --- | --- |
| 20 ms | 0/0 | 76 / 76 | 2 | 0 / 0 |
| 20 ms | 1/1 | 38 / 38 | 1 | 0 / 0 |
| 20 ms | 2/2 | 0 / 0 | 0 | 0 / 0 |
| 40 ms | 0/0 | 114 / 114 | 3 | 0 / 0 |
| 40 ms | 1/1 | 76 / 76 | 2 | 0 / 0 |
| 40 ms | 2/2 | 38 / 38 | 1 | 0 / 0 |
| 150 ms | 0/0 | 259 / 259 | 8 | 118 / 118 |
| 150 ms | 1/1 | 250 / 250 | 7 | 69 / 69 |
| 150 ms | 2/2 | 248 / 248 | 7 | 30 / 30 |

In the impaired 0/5 case, P1 replayed 4 frames and P2 replayed 975. Reversing the delays produced 943 / 4. That illustrates the effect on the opponent. Exact counts depend on packet scheduling; these are observations from one run per configuration.

The high-latency zero case stayed correct but took about 7.1 seconds for 300 frames, rather than the nominal five seconds. It is not evidence of smooth play at 300 ms RTT. Even the zero-added-latency case can predict in this fixture because packet forwarding and game polling have distinct phases.

The negative control deliberately preserves the wrong digest when loading an earlier frame. It returned exit code 1 with `passed:false` despite both peers confirming input frame 179. The corresponding correct-restore control passed. This catches the false assurance of a frame-counter-only test. Receipts are in `build/delay-audit/`; all four selected CTest targets passed: the new fixture, confirmed-input, simulation-gate and pacing-controller tests.

This fixture does not execute the native game, render, play sounds, traverse the helper/Internet route, model two machines with different CPU costs, or enable Ember's continuous pacing. It isolates GGPO's input and state-recovery behavior. A separate pacing experiment examines the controller.

## Clock synchronization with unequal delays

The pinned fork's `UdpProtocol::SetLocalFrameNumber` estimates the remote frame from the last received input's frame number plus an RTT-derived term. That input frame already includes the remote player's delay. Ember's continuous controller uses half the difference between the two reported frame advantages. These reports therefore include input-delay differences as well as simulation-clock differences.

Under symmetric steady transport, the inferred signal is approximately the actual simulation-frame gap plus half the local-minus-remote input-delay difference, with polling, quantization and reporting lag. For 0/5, its balance point can put the zero-delay side ahead by roughly 2.5 simulation frames. This behavior is not a deterministic desync, but it means the controller's logged `riftFrames` cannot be interpreted as a pure clock gap with unequal settings.

`RecoveryBenchmark --rift` now accepts `--second-input-delay` and records a signed actual-frame-gap sample over its last 300 ticks. This is a pacing diagnostic; its frame-only saved state is not a correctness oracle. Both measured clocks use 60 Hz, a 20 ms per-direction proxy delay, no loss, and the current continuous controller. Results are retained as `build/delay-audit/rift-*.json`.

All five 20-second pacing runs completed without reported fatal errors or prediction stalls:

| Delays P1 / P2 | Final simulated frames P1 / P2 | Replayed frames P1 / P2 |
| --- | --- | --- |
| 0/0 | 1200 / 1200 | 300 / 300 |
| 1/1 | 1201 / 1201 | 152 / 150 |
| 0/1 | 1201 / 1200 | 150 / 300 |
| 0/5 | 1201 / 1199 | 0 / 19 |
| 5/0 | 1199 / 1201 | 19 / 0 |

The 0/5 and 5/0 runs shifted the relative simulation clocks in opposite directions, consistent with the delay contribution in the estimator. Their much lower replay counts than the equal-clock correctness fixture also show why the controller's policy matters. Final counts and signed samples have tick-phase quantization; these five short runs do not establish an exact steady-state lead or native fairness. This diagnostic does not test native simulation cost.

Do not remove the delay contribution from the controller solely because it looks like an offset: it changes where prediction work falls. The desired fairness and smoothness policy needs to be specified and checked on both native clients before changing that behavior.

## Native acceptance still required

Use matching game/mod hashes and keep the route, stage, characters and display settings fixed while comparing 0/0, 1/1, 0/1 and 1/0. Add 0/5 and 5/0 to expose pacing asymmetry. Repeat through round transitions, rematches, projectiles, supers, character state changes and brief recoverable network interruptions. Capture both clients' `sf4e.log` files, not only the helper session logs.

Enable `SF4E_ROLLBACK_DIAGNOSTICS=1` for timing evidence. Compare confirmed semantic checkpoints, engine/GGPO frame offsets, save/load failures, maximum rollback depth, replay counts, prediction stalls, frame overruns and pacing shifts. Record visible correction and audio behavior separately. A zero-desync result alone does not pass the smoothness comparison.

For local restore coverage, the existing offline stress mode supports `SF4E_ROLLBACK_STRESS=1..8`, `SF4E_ROLLBACK_STRESS_PREDICT=3` and diagnostics. It deliberately rolls back and compares repeated corrected timelines. Exercise it in Versus or Training with gameplay inputs; it is not intended for ordinary online matches. Character coverage and native execution costs remain necessary even when synthetic tests pass.

The next changes should be driven by this evidence: repair any reproducible restore/frame-accounting failure first; clarify the meaning of delay and its effect on both peers; decide the default separately; and validate or refine asymmetric pacing before presenting the feature as generally stable. Raising the prediction limit, suppressing desync detection or promising that zero is always smoother would not resolve the demonstrated tradeoffs.

## Reproduction

Build `GgpoDelayRollbackTest` and `RecoveryBenchmark` in the configured no-Discord development build. The fixture's positional arguments are P1 delay, P2 delay, added one-way milliseconds, jitter milliseconds, drop interval, frame count, input hold interval and corrupt-restore switch. For example:

```powershell
./build/current-no-discord/GgpoDelayRollbackTest.exe 0 0 20 0 0 300 8
./build/current-no-discord/GgpoDelayRollbackTest.exe 0 5 40 30 17 300 1
# Negative control: expected exit code 1 and passed:false, with confirmed inputs.
./build/current-no-discord/GgpoDelayRollbackTest.exe 0 0 25 0 0 180 8 1
./build/current-no-discord/RecoveryBenchmark.exe --rift --frames=1200 --fast-hz=60 --delay-ms=20 --drop-every=0 --input-delay=0 --second-input-delay=5
```

Run timing experiments sequentially. The benchmark's `measured` field must be checked, as its process exit code alone does not establish a valid measurement.
