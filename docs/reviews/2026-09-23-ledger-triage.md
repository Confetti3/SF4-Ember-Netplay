# Ember master issue ledger triage (2026-09-23)

Every item in `EMBER_MASTER_ISSUE_LEDGER_2026-09-23.md`, checked against `sf4-current` at 1ad20d2 (v0.9.7 plus the README commit, branch experiment/limiter-pacing). No source was changed. Paths are under `src/` unless they start with `rust/`, `docs/` or `scripts/`.

Verdicts:

- **LIVE**: the defect is in the current code as described.
- **PARTIAL**: fixed in part; the gap is named.
- **FIXED**: fixed; the commit and test are named when there is one.
- **OBSOLETE**: the code path is gone.
- **UNCLEAR**: the code allows it but nothing reproduces it yet.
- **FIELD**: no code-level cause found; the next measurement is named.

Most recent fixes landed in 6ad4e98 (v0.9.6-rc2, "Fix v0.9.6-rc1 review findings"). `sf4e__Application.cxx`, which the older audits cite, is now `sf4e/sf4e__NetplayRuntime.cxx` (00b5f2f).

## Fix status (branch fix/ledger-triage)

Fixed with a regression test named by ID:

- **Rollback save and sound:** A-001, A-003, A-006.
- **Room lifecycle:** A-005, N-005 (logged, no test), H-005 (no test), H-006, H-008.
  - H-008 was worse than the triage below says. When a follower's game dies and the leader leaves within the 15 s grace, the helper could hand authority to the dead voter. The survivors then had no quorum. Both halves are fixed, and the fixture scenario `killed-then-leader-leaves` fails on the old code and passes on the new.
- **Updater:** A-013, H-013.
- **Pacing:** A-012, A-007 (the limiter sleep; `SF4E_LIMITER_SPIN=1` restores the old spin).
- **Diagnostics:**
  - H-010, H-012, H-014 are fixed.
  - A-010 and F-006 only log counts, and have no test.
- **Build gate:** A-015 (cargo tests are now in ctest as `HelperRust`).

Corrected: A-016 needs no change. `GetHash` writes the hash only on success, and `IrohRoom::Host` and `Join` refuse an empty build. So a Sidecar that failed to hash cannot open or join a room, and the caveat below does not apply.

Nine read-only Codex (gpt-6-sol) audit passes reviewed the branch. Every accepted finding is fixed on the branch, and pass 9 had no findings. These were declined, with the reason given to the auditor:

- **H-010:** a ping that stays stale while the link is not interrupted needs a quality-reply timestamp from GGPO, which means a port patch.
- **H-013:** a new Launcher paired with the old Updater during the one-time upgrade from v0.9.7 can leave the player at the desktop once. Recovery stays safe and idempotent.
- **H-008:** voter reachability is sampled just before the membership awaits. OpenRaft 0.9.25 has no heartbeat metric to re-check it.
- **A-015:** the network fixtures stay out of the build gate. The receipt now names them in `testsExcluded`.

F-008 (Linux) has new logs. On Proton the room closes 30 s after a normal match end on `match_teardown_timeout`, because the helper's `game_closed` never reaches the match session. The room now logs each `end_match`, each `game_closed` and `game_failed` it receives, and at the timeout the per-link state and the helper's load. The root cause is still open until a log from this build arrives.

Still open, with the reason:

- A-008 (engine memento payload sizes need IDA, which was unavailable).
- A-009 (a smaller art budget changes image quality; needs your visual check).
- H-009 residual (whether the game's Reset wrapper's return value means success is unknown without IDA).
- H-004 (not reproduced; every recovery fixture scenario passes, and N-005 now logs the reason if it recurs).
- All FIELD items (they need the measurements listed below).

## Summary

| ID | Verdict | One line |
|---|---|---|
| F-001 | FIELD | Scoreboard change is display only; better leads are A-005, H-005, H-006, H-008, N-005 |
| F-002 | FIELD | Rollback only; A-001 gap and the 0-worker job change are the code-side suspects |
| F-003 | FIELD | Separate state machine from F-002; needs a forced-rollback repro |
| F-004 | FIELD | Needs a forced-rollback repro |
| F-005 | FIELD | Needs a crash dump; A-002 stub is gone, so it is not the cause |
| F-006 | FIELD | No change to the meter or panel since v0.9.4; suspect 62dcc00 |
| F-007 | FIELD | Ember has no replay hooks; three plausible indirect causes |
| F-008 | FIELD | Blocked on Linux logs; N-005 makes those logs less useful than they should be |
| F-009 | FIELD | Reset path is mostly fixed (H-009); no spectator-to-Reset link found |
| F-010 | FIELD | Abrupt-exit paths exist; H-008 gap is the best code lead |
| F-011 | FIELD | Needs per-phase owner-thread timing in 4 and 6 member rooms |
| F-012 | FIELD | A-003 allocates on every save; measure first vs later match |
| F-013 | FIELD | A-007 spin is the main non-simulation CPU cost |
| F-014 | FIELD | A-008, A-009, A-010 make memory unmeasurable today |
| A-001 | PARTIAL | Online save aborts on a bad functor; training, stress and restore do not check |
| A-002 | FIXED | Blocking stub hook removed in 6ad4e98 |
| A-003 | LIVE | Inner sound vectors are destroyed and reallocated on every save |
| A-004 | FIXED | 60 s room-end deadline, tested |
| A-005 | PARTIAL | Write errors release the table; a refused queue still holds it forever |
| A-006 | LIVE | New adapter is never marked claimed |
| A-007 | LIVE | Limiter spins; pacing lengthens the spin |
| A-008 | LIVE | `*len = 1`, no byte accounting |
| A-009 | LIVE | Budget ignores managed copies and decode queue; eviction can stall above budget |
| A-010 | LIVE (by design) | Reclaim drops payloads on purpose; nothing counts them |
| A-011 | FIXED | Continuous mode clears debt on stall; coarse mode keeps it by design |
| A-012 | LIVE | No generation fence; relies on an unasserted same-thread assumption |
| A-013 | PARTIAL | Extraction is bounded and cancellable; dead INFINITE wait and no cleanup remain |
| A-014 | FIXED | Core-only CMake order fixed, CI builds it |
| A-015 | LIVE (by design) | CI does not build the product; the local receipt is the gate |
| A-016 | FIXED | Docs say versions must match; hash check present (one caveat) |
| H-001 | FIXED | One `LiveMatchRoster` feeds grant and projection, tested |
| H-002 | FIXED | Early connect is stored and consumed, tested |
| H-003 | FIXED | Policy drops before ring capacity; GGPO patch disconnects on full ring |
| H-004 | LIVE | Two recovery failures in `docs/perf/HANDOFF.md` were never closed |
| H-005 | PARTIAL | Helper leave is bounded; runtime room-Leave retry loop is not |
| H-006 | PARTIAL | Room actions and Ready are parked; three command kinds are still dropped |
| H-007 | FIXED | Stall notice at 10 s, Replace offered at 30 s |
| H-008 | UNCLEAR | New leader never arms departure for members already absent |
| H-009 | PARTIAL | Reset is handled and tested; failed Reset and render-thread join remain |
| H-010 | LIVE | Ping freshness follows local query success, not remote samples |
| H-011 | FIXED | Scales stay distinct at 720p and 480p (2c53bf4) |
| H-012 | LIVE | Validator accepts 30 samples from any capture length |
| H-013 | PARTIAL | Journaled install and recovery exist; Launcher never runs recovery |
| H-014 | LIVE | Training capture and session trace fail silently |
| P-001 | Constraint | Still true; see F-011 |
| P-002 | FIXED | Idle room does not build checkpoints (e0b91a2) |
| P-003 | FIXED | Frozen members and unresolved results no longer wake the room (e0b91a2) |
| P-004 | Constraint | Test requirement; keep direct and relay separate |
| N-001 | FIXED | Admission and game accept run in spawned, timed tasks |
| N-002 | FIXED | Admitted members ignore invite expiry (ee28f02) |
| N-003 | FIXED | Game proof paths are deadline-bound (ee28f02) |
| N-004 | FIXED | Route updates flow to C++ each second (ee28f02) |
| N-005 | PARTIAL | Helper keeps five reasons; the runtime overwrites and clears them |

## Room lifecycle (the F-001 / F-010 cluster)

**F-001 and the scoreboard.** 2c53bf4 reads the existing `room.tables[m.table].score` into `NetplayFacade` status and draws it (`ui/Theme.cxx` `SetScoreText`, layout in `RoomPanel.cxx`, `ApplicationShell.cxx`, `sf4e__Overlay.cxx`). It does not touch the room model, generations, result persistence or any queue. No per-match container grows without bound: terminal receipts cap at 64, ack tombstones at 256 (`session/RoomModel.hxx:401-404`), the effect journal at 256 entries (`session/SessionRecovery.hxx`), action and event queues at 32 or 64. The scoreboard is an unlikely cause. The items below can each end a room after several matches, and N-005 is why the logs from those rooms do not say which one happened.

**A-005: PARTIAL.** 75d0f59 releases the terminal receipt when the settings writer reports an error (`sf4e/sf4e__NetplayRuntime.cxx:1443-1452`). But `OverlayPrefs::QueuePlayerPreferences` returns 0 when the writer is null or the preferences are invalid (`sf4e/sf4e__OverlayPrefs.cxx:218`). Then `terminalPersistRevision` stays 0 and the block at `NetplayRuntime.cxx:1428-1435` retries every tick with "match record pending". The terminal ack is never sent, so the whole table stays held. Reachability of a null writer or invalid preferences in the field is not proven. Tests: `MatchResultOutboxTest` covers `PrepareProfileConsumption` only; nothing covers the runtime release path.

**N-005: PARTIAL.** `rust/sf4-net/src/bridge.rs` has five failure reasons and `IrohRoom.cxx:242-247` maps them to `gameplay_<reason>`. But `IrohMatchSession::Fail` (`session/IrohMatchSession.cxx:28`) does not log. In the recoverable path `TickMatch` (`NetplayRuntime.cxx:1627`) sets the generic `runtime.match_connection_lost`, then calls `match->Abort()`, which runs `error_.clear()` (`IrohMatchSession.cxx:243`). The trace samples `match->Error()` later (`NetplayRuntime.cxx:513`), so it is already empty. The reason survives only when `CloseRoom` logs it (`:185`), which is the teardown-timeout path.

**H-005: PARTIAL.** `IrohRoom::Leave` has an 8 s timeout and falls back to leaving locally (e656583). The runtime loop in `SettleRoomState` (`NetplayRuntime.cxx:1772-1781`) resends a room `Leave` every 500 ms while the room is writable, the member is present and no accepted reply arrived (`:1461`). It has no deadline. An authority that keeps rejecting the Leave holds the client in Closing.

**H-006: PARTIAL.** RoomAction presses are parked while the fence is closed (`NetplayRuntime.cxx:1238-1244`, 3 s deadline at `:1721-1726`). Ready and Rematch are parked on rejection (`:1249-1253`). The StaleTable resend from rc3 and `CatchUpGraceMs = 250` (`netplay/SessionController.cxx`) are in place. Still dropped:
- Any other command the controller rejects hits `continue` at `:1253`, including SetLobbySettings when it is not already parked at `:1183`, ApplyDelay and CheckConnection.
- A RoomAction during `recoveringMatch` skips parking (`:1240`).
- A parked RoomAction is resubmitted only at the main menu with the match Idle (`:1754`), so one parked mid-fight times out after 3 s. Unqueue and Unwatch for a spectator are the exception (`:1742`).

**H-008: UNCLEAR, likely live.** A member whose control closes gets a 15 s `departureDeadline` (`session/IrohRoom.cxx:728`, 7a2dfaf), and `ExpireDepartedPeers` turns that into a committed Leave (`:660-670`). On `control_rebound`, a new leader clears deadlines for connected peers (`:400`) and creates peer entries for every roster incarnation (`:409-416`), but never arms a deadline for a member that is on the roster and not connected. A follower that takes over only ever had a control edge to the old leader, so every member that died before the takeover is never pruned. The helper's 30 s `expire_departure_grace` (`rust/sf4-net/src/service/members.rs:731`) covers only the member that is leaving. This fits the open "ghost member in 2-member rooms" note from the 2026-09-21 fix. Needs a fixture before a fix: kill a follower, then kill the leader, then check that the survivor removes the first follower.

**H-004: LIVE.** `docs/perf/HANDOFF.md:18-28` records run 5 (timeout at "fresh generation after preparation mappings retired") and run 6 (`gameplay_connection_lost phase=6`, from `IrohMatchSession.cxx:444-448`). No later note gives a cause. Coverage is only the wall-clock `IrohRecoveryIntegrationTest` scenarios. N-005 would have named the run 6 reason.

**Fixed with tests:** A-004 (`session/MatchTeardownTiming.hxx`, `IrohMatchSession.cxx:384-387`, `TestMissingRoomEndIsBounded`); H-001 (`RoomAuthority::LiveMatchRoster`, `session/RoomModel.cxx:288-293`, `TestRetiringSpectatorProjection`); H-002 (`IrohMatchSession.cxx:197,307-310`, `TestEarlyConnectSameTick` / `SeparateTicks`); H-003 (`common/SpectatorPolicy.hxx:26-31`, drop at 48 frames with 200 ms sampling stays under 63, plus `pending-output-disconnect.patch`; the margin assumes about 60 fps polling); H-007 (`netplay/SessionController.cxx:73-83`); P-002 and P-003 (`session/sf4e__SessionServer.cxx:184-190`, `sf4e__SessionServer__Rooms.cxx:152-157`); N-001 to N-004 (`rust/sf4-net/src/service/members.rs:255-310`, `games.rs:28-84`, `invite.rs:361-362`, `mod.rs:1086-1101`, each with a `service/tests.rs` or `transport/tests.rs` case).

**F-010 paths.** `fMain::Destroy` (`sf4e/sf4e__Platform.cxx:319-323`) runs `ShutdownNetplay` then `StopHelper`. `StopHelper` (`NetplayRuntime.cxx:756-789`) closes the room, waits 1200 ms for departure and sends shutdown. On the leader, `IrohRoom.cxx:715-729` and `ExpireDepartedPeers` remove the dead member. The fixture is `IrohRecoveryIntegrationTest --scenario=follower-killed`. H-008 is the gap.

## Rollback, sound and pacing

**A-001: PARTIAL.** `RecordFunctor` (`sf4e/sf4e.cxx:250-261`) rejects size 0 and oversize functors, and task data and task count are capped (`:353-376`). All of these set the global `fTaskCore::recordFailed`. Only the GGPO save callback resets and checks it (`sf4e/sf4e__Game__Battle__System__Ggpo.cxx:622-628`). Unchecked callers:
- the training checkpoint (`training/TrainingRuntime.cxx:72`)
- the stress harness (`sf4e__Game__Battle__System__RollbackStress.cxx:153`)
- `sf4e__Game__Battle__System.cxx:610`
- the round-trip free (`sf4e__Game__Battle__System__SaveState.cxx:513`)

On restore, the task-data size check is assert-only (`sf4e.cxx:392`) and `AllocateNewTask`'s result is not checked (`:396-402`). A training checkpoint with an unknown functor restores that task without its functor and shows no error. No tests.

**A-002: FIXED** in 6ad4e98. The `Sound::Unit::IsStillPlaying` stub is unhooked (`sf4e/sf4e__Game__Battle.cxx:48-49`). The remaining `CriPlayerAdapter::IsStillPlaying` hook (`:466-478`) reads `bLive` through `operator[]`, which inserts a map entry on a miss. That is harmless but untidy.

**A-003: LIVE.** `SaveState::Clear` calls `managerState.clear()` (`SaveState.cxx:364`), which destroys each `SoundObjectPool::SaveState` and its two vectors (`sf4e/sf4e__Platform.hxx:71-74`). The next save `emplace_back`s fresh ones (`SaveState.cxx:688`) and `push_back`s into them. The constructor comment at `SaveState.cxx:39-42` ("these never allocate again") is only true of the outer vectors.

**A-006: LIVE.** In `SyncState` (`sf4e/sf4e__Game__Battle.cxx:392-443`), a matching live adapter is claimed at `:414`. The PlaySound branch (`:418-443`) sets `targetAdapter` from the new handle and never claims it. Ember's own PlaySound hook marks that adapter live, so a second identical stub sound matches it. Two stub sounds then drive one real adapter. The inner loop also bounds real adapters by the stub manager's count (`:400`).

**A-007: LIVE.** This rests on source comments only, because the IDA connection was down. The native limiter spins to its deadline (`Dimps/Dimps__Platform.hxx:86-87`, image offset 0x370de0). The hook (`sf4e/sf4e__Platform.cxx:145-183`) swaps the period at +0x1f8 for one call. The high-resolution waitable timer was removed in f8bfcbb, and VSync is forced off (`:189-197`), so the spin is the only limiter.

**A-008: LIVE.** The GGPO save callback sets `*len = 1` (`Ggpo.cxx:605`). Diagnostics count keys and sound records (`SaveState.cxx:708-717`), not bytes.

**A-009: LIVE.** `ui/SelectionArt.cxx`:
- 32 MiB budget and 512 px images (`:24-25`); the backdrop is 2048 px (`:181`).
- Textures are `D3DPOOL_MANAGED` (`:197-198`), and the system-memory copy is not counted.
- `textureBytes` counts only uploaded pixels (`:221`). Up to 4 completed decodes and 128 queued jobs are not counted.
- Eviction stops when nothing is older than 2 frames (`:226-228`), so usage can stay over budget.

**A-010: LIVE by design.** `Reclaim` (`SaveState.cxx:367-386`) logs "reclaiming leaked slot", sets `ownsKeys = false` and clears without freeing. `savestate_ownership_test.cxx` asserts that no payload is released. The missing piece is a count of abandoned states and bytes, not a code change.

**A-011: FIXED** for continuous mode in 6ad4e98. `OnPredictionStall` zeroes outstanding debt (`common/sf4e__PacingController.hxx:138-144`), covered by `RiftHoldsAfterStall` in `tests/degraded-connection/policies_test.cxx`. Coarse mode (`SF4E_CONTINUOUS_TIMESYNC=0`) keeps its debt by design.

**A-012: LIVE.** `CancelFrameShift` zeroes two atomics (`sf4e/sf4e__Platform.cxx:137-140`). A `LimitFrame` that already took its request (`:153`) still adds its applied shift afterwards (`:180`). The comment at `:76-79` says this is safe because the limiter and the pacing tick share a thread. Nothing asserts that.

## Build, updater, UI and telemetry

**A-013: PARTIAL.** `RunProcessAndWaitHidden` (`launcher/update/github_release_client.cxx:19-48`) waits in 250 ms steps, honors cancel, gives up after 5 minutes and checks the exit code (`:72-77`). Remaining gaps:
- `launcher/updater.cxx:165-183` still defines `RunProcessAndWait` with `INFINITE`, and nothing calls it.
- There is no job object, so only `tar.exe` itself is killed, not any child processes.
- The extract folder (`%TEMP%\sf4-netplay-update-<tag>\extract`), the downloaded zip and the `sf4e-updater-*` copies are never removed.

**A-014: FIXED** in 6ad4e98. `CMakeLists.txt:46-48` returns before `RoomMessageQueueTest` (`:78-80`) in core-only mode, and CI's `core-tests` job builds that mode.

**A-015: LIVE by design.** `.github/workflows/degraded-connection-experiment.yml` runs pacing tests, GGPO patch verification and core-only tests. The product gate is local: `scripts/package-team.ps1` requires a `build-provenance.json` receipt with `testsPassed` and matching HEAD, checked by `Assert-BuildReceipt` (`scripts/BuildProvenance.ps1:37-54`). `cargo test` and the network fixtures are not part of that receipt.

**A-016: FIXED.** The release notes, `README.md:70` and `docs/guides/CUSTOM_ROOMS.md` say versions must match. Admission checks the hash in C++ (`session/sf4e__SessionServer.cxx:479-481`) and in Rust (`rust/sf4-net/src/invite.rs:361-368`). Caveat: if hashing the Sidecar fails, `sf4e::Install` shows a message box and continues with the default hash (`sf4e/sf4e.cxx:224-227`), so two clients that both failed would match.

**H-009: PARTIAL.** The Reset hook (`sf4e/sf4e__Platform.cxx:209-217`) frees and recreates the overlay. Focus handling works while no context exists (`sf4e/sf4e__Overlay.cxx:319-335`). Covered by `overlay_reset_test.cxx`, which includes a real DX9 Reset. Remaining gaps:
- The hook ignores Reset's return value and rebuilds the overlay after a failed Reset.
- `~SelectionArt::Impl` joins the decode worker on the render thread (`ui/SelectionArt.cxx:129-133`), so a Reset can wait on a 2048 px decode.
- No test covers a failed Reset or a fullscreen/windowed switch.

**H-010: LIVE.** `common/MatchTelemetry.hxx:16-17` treats ping as fresh while the last successful local stats query is under 2 s old. `ggpo_get_network_stats` succeeds whether or not a new round trip arrived. The HUD also shows 0 ms before the first round trip; v0.9.7 skipped 0 only in the log (`Ggpo.cxx:31-33`).

**H-011: FIXED** in 2c53bf4. `ui/Theme.cxx:288-295` floors the viewport factor at 0.8, giving 0.68, 0.80 and 1.00 at both 720p and 480p. `ui_render_test.cxx:563-569` checks the ordering but not at 640x480.

**H-012: LIVE.** `scripts/capture-frame-pacing.ps1:18,29-35` accepts any capture with 30 rows and 30 usable intervals. It never compares usable samples to the row count or duration.

**H-013: PARTIAL.** Installs are journaled and roll back (`launcher/update/PackageInstaller.cxx:102-240`), with a forced-termination test (`package_installer_test.cxx:85-114`) and `UpgradeRecoveryPowerShell`. `RecoverPackage` is called only from `updater.cxx:253` (`-RecoverOnly`) and inside the next install. The Launcher never checks for a pending journal at start, so after an Updater crash the game can start on a half-updated install. `.ember-update-backups/` is never pruned.

**H-014: LIVE.** `training/TrainingCapture.cxx:31-62` returns silently when the folder or file cannot be opened and never checks writes. `common/SessionTrace.hxx:29-40` returns silently on a failed open, and `Record` then reports success.

## Field items: next measurement

- **F-002, F-003, F-004, F-005 (Cammy, Viper, Yang).** Close A-001 first, so any unknown functor is logged with its vtable in every save path. Then run each move under `RollbackStress` with a forced rollback at startup, first active frame and the cinematic, and log the vtables it reports. F-002 has a clean no-rollback control, so a hash diff of the fighter and task state before save, after restore and after re-sim is the next step. F-005 needs a crash dump.
- **F-006 (frame advantage).** `git log v0.9.4..v0.9.7 -- src/training src/ui/TrainingPanel.cxx` is empty. The meter only produces a value when it sees exactly one frame of progress per update (`training/TrainingRuntime.cxx:103-104`, `training/FrameMeter.hxx:105`). 62dcc00 (v0.9.5) forced battle jobs onto the game thread (`sf4e/sf4e__Game__Battle.cxx:55-69`) for all modes, including training. Log the frame delta in `AfterUpdate` with the JobManager hook on and off. If the delta is ever 0 or 2, that is the regression.
- **F-007 (replays).** Ember hooks no recorder. There are three indirect candidates, and one stock/Ember A/B split tells them apart:
  - replays recorded with engine workers but played back with 0 workers, or the reverse (62dcc00);
  - pad hooks that mask or substitute input (`sf4e/sf4e__Pad.cxx:26-100`);
  - netplay rounds, time and edition settings written into the PvP settings and never restored (`sf4e/sf4e__UserApp.cxx:84-88`).
- **F-008 (Linux).** Needs Waldo's Proton logs (`compatdata/45760/pfx/drive_c/users/steamuser/AppData/Roaming/sf4e/logs`). Fix N-005 first so they name the failure.
- **F-009 (spectator after display switch).** No code ties spectator setup to Reset. Reproduce with a Reset during spectator setup; check the H-009 failed-Reset path.
- **F-011, F-012, F-013.** Time each `TickRuntime` phase (the 16 phase functions) at p50/p99 in 2, 3, 4 and 6 member rooms. Measure the limiter spin (A-007) separately from simulation time. For F-012, count allocations in the first match against a rematch; A-003 is a known per-save source.
- **F-014 (RAM).** Nothing reports rollback bytes (A-008), texture bytes including managed copies (A-009) or abandoned payloads (A-010). Add those three counters before cutting anything.

## Security checklist (ledger section 7)

Guards are present for:
- room joins: 32-byte capability, constant-time compare, build and expiry checks (`invite.rs:354-371`)
- spectator checkpoints: SHA-256 verified, committed through Raft
- results: both fighters must agree (`session/RoomModelActions.cxx:204-225`)
- stale connection IDs: incarnation checks
- message sizes: 64 KiB control, 512 KiB IPC, 1 MiB checkpoints
- handshake exhaustion: 38 tasks, 10 s timeout (`service/mod.rs:1044-1059`)

Partial:
- The stable Iroh identity replaces the claimed user, but falls back to the claimed value when the stable one is empty (`session/sf4e__SessionServer.cxx:496-500`).
- There is per-poll backpressure but no per-member message rate limit.
- Direct QUIC paths expose IPs to connected peers, which is inherent to peer-to-peer.

Obsolete: shared relay slot hijack (the relay is gone; relays are an allowlisted set), and unauthenticated lookups extending room lifetime (there is no lookup server). Authority from vector order survives only in unreachable legacy `clients[0/1]` code.

## Fix queue

Ordered by player impact, then by how much each fix helps diagnose the rest. Each fix goes in its own commit with a regression test named by ID.

`sf4e/sf4e__NetplayRuntime.cxx` is already 1900 lines and holds A-005, H-005, H-006 and N-005. Those fixes should move logic out of it, not add branches to it. `session/IrohRoom.cxx` is at 907.

1. **A-005.** Move terminal outcome consumption (`NetplayRuntime.cxx:1415-1455`) into `netplay/MatchResultOutbox` as one step that returns Consumed, Waiting or Released. A refused queue counts as Released, just like a write error. That removes the fourth path instead of adding a branch for it, and it makes the release path unit-testable without the runtime.
2. **N-005.** Keep the reason. Have `IrohMatchSession::Abort` keep the last failure (or have `Fail` log it once), and pass `match->Error()` into `ReportMatchAbort` and `AbortGgpoMatch` instead of the generic string. This comes before other room work because it makes every later field log name its cause.
3. **A-006.** Claim `targetAdapter` once, after both branches. Bound the inner loop by the real manager's count. Test: two identical live stub sounds map to two adapters.
4. **A-001.** Make `SaveState::Save` return whether the record is complete, `[[nodiscard]]`, and drop the global `recordFailed` flag. Each of the five callers then has to decide what to do, and the restore size check becomes a real check. Test: an unknown vtable in each save path.
5. **H-008.** Fixture first (follower dies, then the leader dies). If it reproduces, arm `departureDeadline` on `control_rebound` for every roster member not in `members`, next to the existing clear at `IrohRoom.cxx:400`.
6. **H-005.** Give the runtime Leave loop the same bounded fallback as `IrohRoom::Leave` (leave locally after the deadline), in one place rather than a new timer beside the existing one.
7. **H-006.** Replace the three parking slots (`pendingRoomAction`, `pendingReady`, `pendingLobbyEdit`) and the drop at `:1253` with one parked-intent model keyed by command kind, extracted from `NetplayRuntime.cxx`. Make the drop or park decision explicit for each kind.
8. **A-003.** Keep `managerState` elements and their inner capacity across `Clear`, and track the logical count separately. Test: no allocation after the first save.
9. **Lower priority:**
   - H-010: tie ping freshness to GGPO's remote sample time, and show nothing until the first round trip.
   - H-014: surface capture and trace open failures.
   - H-012: require coverage of the capture length.
   - A-013: delete the dead `RunProcessAndWait`, use a job object, clean up staging.
   - H-013: run `RecoverPackage` at Launcher start; prune old backups.
   - A-016: refuse to host or join without a sidecar hash.
   - A-012: assert the shared thread ID.
   - A-015: add `cargo test` to the build receipt.
   - A-008, A-009, A-010: add byte counters.
