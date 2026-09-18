# Handoff: room performance and spectator isolation (2026-09-18)

All work is uncommitted in `sf4-current` (`git status` lists it). Nothing is pushed or released.
Background and numbers: `docs/perf/ROOM_PERF_BASELINE.md`.

## State

Built and passing:

- All non-network unit tests.
- `cargo test --lib` in `rust/sf4-net`: 77 pass. It needs a vcvars x64 environment and
  `CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS=-C target-feature=+crt-static`.

Network fixtures on the final binaries: four-table, 14-spectator, `IrohAuthorizedMatch`,
`IrohAuthorizedMatch --terminal-recovery` and relay recovery pass. `IrohRecoveryIntegrationTest`
passed 4 of 4 so far.

Recovery result from the first session: runs 1 to 4 passed; run 5 timed out in "fresh generation after preparation
mappings retired" (the match never reached Started after a leader loss). That one is not yet
diagnosed. Earlier it also appeared once, on a build without the Ready fix, so it may predate these
changes. Read `verify-logs\recovery2-5.log` in the scratchpad and compare against a baseline build
(the memory note says how to build one) before release.

Later the same day: the stalled-spectator fixture (`--members 6 --single-table --cycles 2
--stall-spectator`) and a second four-table run pass. Six more recovery runs: 5 passed, and run 6
failed with `Recovery match 2 failed: gameplay_connection_lost phase=6` before the post-result
scenario printed "Started recovery generation". The run-5 stall did not recur. The recovery
fixture's timeout dump now also prints the proposal and staging state (in flight, status, wire
identity, sent/acked, staged, parked marker, pending proposal, match phase).

A baseline for comparison is configured and built in a detached worktree at HEAD in that session's
scratchpad (`base` and `base-build`, reusing `build\current\dependencies`, Discord off). One
baseline recovery run passed; the A/B comparison was stopped there. Clean up with
`git worktree remove --force <path>` or `git worktree prune` once the folder is gone.

## To finish

1. Decide whether the two recovery failures predate these changes: run the baseline and the current
   `IrohRecoveryIntegrationTest` alternately (about 3.5 min per run). Run network fixtures from a
   folder holding copies of `build\current\*.dll`, the exe and `sf4-net.exe` (0xC0000135
   otherwise), detached with `Start-Process pwsh`.
2. Run the full `scripts\build-current.ps1 -VisualStudioPath 'C:\Program Files (x86)\Microsoft
   Visual Studio\18\BuildTools' -DiscordSdkArchive C:\Users\Kate\Downloads\DiscordSocialSdk-1.10.19337.zip`
   once (provenance, GGPO port-version 8 hash, staging). Do not edit sources while it runs.
3. Optional: measure helper tick lag on a 2-member `--perf` run. The only 16-member readings were
   taken on a contended machine, and they decide whether the bridge needs its own runtime (plan step 9).
4. Commit. Suggested split: bug fixes; recovery decode worker and host cost; spectator isolation
   with the GGPO patch; instrumentation, harness and bench.

## What changed

Bugs fixed (pre-existing):

- A chat line whose sender left made every later snapshot and checkpoint unreadable. `Leave`
  now prunes the lines, and the parser drops orphaned lines (`RoomModel.cxx`).
- A committed effect replayed after a leader handoff was rejected by the term fence
  (`IrohRoom::AuthorizedEffect`).
- A Ready resent after a stale-table rejection answered under a new id and lost the caller's input
  delay (`SessionClient`).

Performance (host cost per commit, worst-case room): step went from 78 to 6.3 ms and propose from
10 to 5.5 ms. Every member's checkpoint decode moved off the game thread (`CheckpointDecodeWorker`,
`SF4E_ROOM_WORKER=0` decodes inline). The changes:

- A duplicate decode is gone.
- Each effect's payload is encoded once, journal sizes are cached, and the proposal is encoded
  directly.
- SHA-256 uses Windows CNG (`RoomDigest.cxx`).
- A projection drops its own replay copy when the journal is full.
- Chat deltas: the join-time capability is `roomChatDelta` and snapshots carry `chat_unchanged`.
- Stale match acknowledgements are dropped before opening a candidate.

Spectator isolation:

- **Teardown:** receipts hold a table only until the fighters acknowledge. A spectator still
  acknowledging sits the next generation out (`RoomAuthority::MatchRoster`).
- **Start:** the fighters-only barrier is negotiated through `spectators_optional` in the grant and
  P1's `game_prepared`. P1 waits `SpectatorGraceMs`, 1500 ms, and lists the ready spectators in
  `game_ready.slots`. The rest get `game_end`.
- **Fighter link closed:** the match session waits `PeerCloseGraceMs`, 2 s, for the committed
  `game_end` before failing.
- **GGPO:** the new patch `spectator-handle-control.patch` (port-version 8) returns spectator
  handles, gives their stats, lets a spectator be dropped, and stops a dropped spectator holding
  RUNNING.
- **Policy:** `SpectatorPolicy` drops a spectator not synchronized within 3 s, or one 32 or more
  frames behind on two consecutive samples. It runs from `fSystem::PollSpectators` in the outer
  tick.

Instrumentation:

- `room.*` timed ops and a `FrameOver` log line.
- The helper's `Statistics` event carries actor tick lag and body time and the event-queue low water.
- `CustomRoomGameTest --perf` and `RoomHostBench`.
- CTest names `RoomPerf.*` and `CustomRoomStalledSpectator` (these need `SF4E_NETWORK_TESTS`).

## Deliberately not done

- Per-table room deltas (snapshots are still about 13 KB).
- IPC event priority classes: the queue never dropped below 123 of 128 free slots.
- A separate bridge runtime or more Tokio workers: not measured on an uncontended machine.
