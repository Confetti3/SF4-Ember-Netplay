# Residual v0.9.8 reports: fixes and code quality review, 2026-09-25

Base: `release` at `9d5e6ac` (v0.9.8). Work branch: 19 commits on top of the base, one per step.
Input: four player reports from the Ember Discord after v0.9.8 and one Linux log archive (`sf4e-logs-098-residual.zip`, build `56e26d9f`), triaged in [2026-09-25-v0.9.8-residual-logs](../validation/2026-09-25-v0.9.8-residual-logs.md).

Scope: the code the four reports touch (`src/sf4e` battle and session lifecycle, `src/session/IrohRoom.cxx`, `rust/sf4-net/src/service`, the GGPO port) plus the crash instrumentation that the unexplained report needs. Not a repository-wide pass; that was done in [2026-09-code-quality-review](2026-09-code-quality-review.md) and its findings still stand.

Standard: the review asks, for every change, whether a reframing deletes a category of branches instead of adding one; whether a file crosses 1000 lines because of it; whether a conditional is scattered into a shared flow; whether a string compare stands where a typed model should; and whether a helper is duplicated. `sf4e__NetplayRuntime.cxx` (1934 lines) gets no new branch; logic that belongs to a report moves out of it.

The rule for the moves: bodies move verbatim, wire formats and user-visible strings do not change, and anything that changes behaviour is listed under "Actual issues" and fixed in its own commit.

## Baseline (before any edit)

| Check | Result |
|---|---|
| `ctest --test-dir build/core` (`SF4E_CORE_TESTS_ONLY=ON`, Linux) | 9 of 9 pass |
| `cmake -S src/tests/degraded-connection` policies tests | 2 of 2 pass |
| `cargo test` (rust/sf4-net, pinned 1.98.0, Linux host target) | 78 of 78 pass |
| `cargo clippy --all-targets -D warnings` | clean |
| `cargo fmt --check` | drift in `service/stall.rs` and `transport/tests.rs` that predates this work; left alone |
| Windows product build | not available on the review machine |

## Findings, ranked

### 1. Structural regression: the decision to leave a fight lived apart from the code that ends it

An online match is a native offline Versus whose pad reads are replaced only while a GGPO session exists (`sf4e__Pad.cxx`, `sf4e__Game__Battle__System.cxx:301` and `:456`). `RetireGgpoSession` reopened `bUpdateAllowed` so that offline battles were never left frozen, and whether the native battle then *left* depended on whether the caller had also written `RS_ISLEAVING`. Seven sites wrote it (`Ggpo.cxx:269`, `:817`, `NetplayFacade.cxx:222`, `System.cxx:601`, `SaveState.cxx:573`, `SessionClient.cxx:169`, `TrainingRuntime.cxx:87`). The retirement paths that did not (closing the room, leaving or replacing it, a room lost during "Synchronizing", which `HandleNetplayFailure` skipped because the flow was still `BF__IDLE`) left the native battle running with the local controller on both slots. That is the "controlling both characters" report (F-016).

Remedy taken: the gate model now records who owns the native battle (`NativeBattleOwner` in `sf4e__GgpoGate.hxx`). The battle is claimed in the one place every netplay battle passes (`_OnVsBattleTasksRegistered`), a claimed battle whose session is retired is orphaned, and `BattleUpdate` drives an orphan out with neutral playback input, re-asserting the exit each update until `CloseBattle`. The exit is now a property of retirement itself, so the `RS_ISLEAVING` writes in `AbortGgpoMatch` and `HandleNetplayFailure` are gone, and no line of `NetplayRuntime.cxx` changed for it.

### 2. Missed simplification: six lifecycle writes to a developer flag

`bUpdateAllowed` was written by `StartGGPO`, `StartSpectating`, `RetireGgpoSession`, `AbortGgpoMatch` (twice) and `HandleNetplayFailure`. Two of those were dead (overwritten by the retire that followed), and one (`AbortGgpoMatch` with no session, the failed-start path) was live in the wrong way: `RetireGgpoSession` returned early without a session, so the flag stayed false and every later offline battle was gated (R3). `MayAdvanceDeterministicFrame` is now `simGate.MayAdvance(sessionLive, bUpdateAllowed)`: without a session only the manual gate applies. All six writes are deleted; the flag is the developer overlay's pause and nothing else.

### 3. Spaghetti: a whitelist string compare decided how much of the room a helper error ends

`IrohRoom::HandleHelperError` was a chain of `code ==` and `compare(0, n, ...)` with a fall-through that failed the whole room for anything unrecognised, including `stale_match` and `invalid_game_registration`, which concern one match. `HelperErrorScope.hxx` is the one table of a code's reach; the room switches on the scope and the branch bodies moved verbatim. Match-scoped errors now keep the room (F-017, second commit). The table is covered on Linux by `HelperErrorScopeTest` for every code the Rust helper emits.

### 4. Spaghetti: the same capacity check written five times, and `?` on every event

The Rust actor guarded droppable events with `self.events.capacity() <= LIFECYCLE_EVENT_RESERVE` in four places (a fifth in `stall.rs` belongs to a separate task and stays), while lifecycle events were `try_send(...)?`, so a full 128-slot IPC queue ended the actor and every gameplay link. `EventOutbox` (`service/events.rs`, 101 lines) owns the queue: lifecycle events that do not fit are held in order and flushed at the start of each tick; bulk events go through `has_headroom()`. `mod.rs` shrank by two lines and stays at 1131; the constant has one reader outside the outbox.

### 5. Boundaries: crash facts never reached the host

GGPO's `ASSERT` showed a `MessageBox` and called `exit(1)`; its text went only to GGPO's own log, which ships disabled. The sidecar had no exception filter, and the asynchronous logger cannot record its own process dying. Three components now share one record: `common/CrashReport.hxx` (pure: the line ring, the exit code table, the header, the address-space fold; tested on Linux), `sf4e__CrashDiagnostics.cxx` (the Win32 filter, the ring sink, the per-match process line), and `assert-handler.patch` on the GGPO port, which routes `Platform::AssertFailed` to a host handler. The launcher logs the exit code through the same table, so there is one place that names `0xC0000005`.

### 6. File size

| File | Before | After | Note |
|---|---|---|---|
| `src/sf4e/sf4e__NetplayRuntime.cxx` | 1934 | 1941 | two comments and a three-line predicate body; no new branch in any flow |
| `src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx` | 848 | 874 | `LeaveOrphanedNetplayBattle` (24 lines) minus the deleted writes |
| `src/sf4e/sf4e__Game__Battle__System.cxx` | 778 | 783 | |
| `src/session/IrohRoom.cxx` | 937 | 945 | the switch is shorter than the chain; two log lines |
| `rust/sf4-net/src/service/mod.rs` | 1133 | 1131 | |
| `src/sf4e/sf4e__NetplayFacade.cxx` | 348 | 340 | |
| `src/sf4e/sf4e__Pad.cxx` | 96 | 88 | two copies of one function became one |
| `src/common/sf4e__GgpoGate.hxx` | 131 | 190 | the ownership model |
| new `src/sf4e/sf4e__CrashDiagnostics.cxx` | | 210 | |
| new `src/common/CrashReport.hxx` | | 113 | |
| new `rust/sf4-net/src/service/events.rs` | | 101 | |
| new `src/session/HelperErrorScope.hxx` | | 71 | |

No file crosses 1000 lines because of this work. `NetplayRuntime.cxx` and `service/mod.rs` were already over it and stay within seven lines of where they were.

### 7. Modularity and legibility

- `GetButtons_MappedOn` and `GetButtons_RawOn` were fourteen identical lines twice; they are one `ReadButtons(pindex, raw)`.
- `HandleNetplayFailure` called `PushAlert(reason)` and `SetLastError(reason)`, which are the same function; one call remains. Its `closeGgpo` argument is always `true` and is recorded, not removed.
- `StickyRuntimeError` compared a translated sentence against the English word "Networking" (R2); it compares against the catalog now.
- The `readyIntent` budget (20 s) was shorter than the helper teardown bound (30 s) it depends on, so the wrong message came first; it is derived from `MatchTeardownTiming::HelperTimeoutMs`.
- `IrohRoom::Fail` set the failed state without a log line; every failure now names its code once, and the control-id rejection logs the ids it compared.

## Actual issues (behaviour changes, each its own commit)

| # | Issue | Evidence | Fix |
|---|---|---|---|
| F-016 | A netplay battle whose session was retired without `RS_ISLEAVING` ran natively with the local pad on both slots | `Ggpo.cxx:239-242` reopened the gate; `CloseRoom`, `Effect::CloseSession`, `Effect::ReplaceRoom` and a room lost during synchronization never wrote the exit | ownership in the gate model; orphan handling in `BattleUpdate` |
| R3 | An abort with no session gated every later offline battle | `AbortGgpoMatch` wrote `bUpdateAllowed = false`; `RetireGgpoSession` returned at `if (!ggpo)` before reopening it | no lifecycle write to the flag; `MayAdvance` ignores the model without a session |
| F-017 | Match-scoped helper errors closed the whole room | `IrohRoom.cxx:830-835` whitelist fall-through | `HelperErrorScope::Match` keeps the room |
| F-017 | A full IPC event queue ended the helper actor | `mod.rs:490-494` `emit()` with `?` everywhere | `EventOutbox` backlog |
| F-015 | Nothing recorded a crash | no filter, GGPO assertion invisible, exit code unlogged | crash record, assertion handler, exit code line, per-match process line |
| R1 | The profile was written two or three times per match | replayed terminal receipts reset `terminalOutcomeConsumed`; `PersistProfile` re-queued | `persisted_` per capture |
| R2 | Sticky errors expired outside English | `NetplayRuntime.cxx:957` prefix "Networking" | compare against `loc::T` |
| F-008 residual | "Ready did not go through" preceded the teardown message | `readyIntent{20000}` versus `HelperTimeoutMs = 30000` | budget derived from the bound |
| F-015 minor | a reserved match port was overwritten without closing | `IrohMatchSession.cxx:190` | release before reserving |

## Checked and dismissed

- F-018, the delay report: the authority computes one delay for both seats (`RoomModel.hxx:168-171`, `SessionServer__Rooms.cxx:257-260`), each client freezes it per generation (`SessionClient.cxx:353-366`), and `iroh_authorized_match_test.cxx:331-423` proves that different local delays stay in sync. Only a log line was added.
- `fKey::trackedKeys` growing across matches as a crash cause: the per-slot key counts in the logs alternate 89 and 90 by matchup and never climb, so the set is not accumulating stale entries. Recorded as a hypothesis the new process line will settle.
- The `DISCONNECTED_FROM_PEER` handler's own `RS_ISLEAVING` write (`Ggpo.cxx:817`): it runs in a gated window before the retire and is not a retirement writer; it stays.

## Considered and rejected

- A vectored exception handler for the crash record: it also sees first-chance and handled exceptions, which the game throws routinely.
- An `atexit` sentinel to detect a silent `exit(1)`: it runs under the loader lock, and the launcher's exit code line gives the same fact.
- Making GGPO assertions non-fatal: GGPO's state after an assertion is not trustworthy.
- A full-memory minidump: over a gigabyte for a large-address-aware 32-bit process; `MiniDumpNormal` names the fault.
- A wire field carrying each side's frame delay (F-018): two log lines say the same.
- One `LeaveBattle()` helper for the five remaining `RS_ISLEAVING` writers: they are the offline, training, desync and developer paths in different layers, and merging them adds an include edge for no behaviour.
- Ownership in `NetplayFacade` instead of the gate model: not pure, not testable on Linux.
- Deleting `bUpdateAllowed` entirely: the developer overlay reads and writes it and needs its own build tree to verify.
- Resetting a known peer's `receivedId` when its control reconnects (a second F-017 candidate: `IrohRoom.cxx:712-714` keeps the id, the sending side restarts at 2 when its peer entry is recreated, and the next message fails the room): kept as the fix to apply once a log shows `Room: control message rejected`.
- Reformatting `stall.rs` and `transport/tests.rs` to satisfy `cargo fmt --check`: the drift predates this work and would bury the change.

## Still open

- `struct Runtime` in `NetplayRuntime.cxx` is still an implicit state machine, and the file is still 1941 lines. Nothing here made it worse; nothing here fixed it.
- `IrohMatchSession` carries teardown state in strings (`error_ == "match_teardown_timeout"` in two places).
- Ten unrelated timers govern leaving a match (2 s peer-close grace, 1.5 s spectator grace, 15 s spectator exit, the Ready budget, 15 s lobby edit, 3 s room action, 30 s helper, 30 s dispute, 60 s room end, 8 s leave). Only the Ready budget is now derived from another.

## Verification

Run on the review machine (Linux, no MSVC, MinGW or Wine):

- Core ctest after every step: 12 of 12 at the end (9 at the baseline; `GgpoGate` moved into the core block, `CrashReport` and `HelperErrorScope` new).
- Policies tests: 2 of 2.
- `cargo test`: 83 of 83 (78 plus the five `EventOutbox` cases). `cargo clippy --all-targets -D warnings` clean.
- `scripts/verify_ggpo_experiment.py`: all 13 port patches apply, `assert-handler.patch` last.

Owed on Windows before any of this ships:

- `scripts/build-current.ps1` and the full ctest, including `MatchResultOutboxTest`, `ControllerNavigationTest`, `RuntimeBootstrapTest` and `SaveStateOwnershipTest`.
- `scripts/verify_ggpo_experiment.py --build` and `build-current-dependencies.ps1`: the GGPO port is at port-version 14 and `GGPO.dll` changes.
- `IrohRoomIntegrationTest`, `CustomRoomGameTest --late-join`, `CustomRoomGameTest --late-spectator`.
- Two-PC runs for F-016: leave the room mid-fight, kill the helper mid-fight, block UDP during "Synchronizing", kill the peer's game. Expected each time: `Netplay battle lost its session; leaving it` in the log, both games back at the menu, and neither side responding to the local pad in between.
- One session of fifteen or more matches to collect the `Process [...]` lines and, if it dies, `sf4e-crash.log`, `sf4e-crash.dmp` and the launcher's `Game exited with code` line.
