# Room performance

Measured 2026-09-18 on one development PC. Two tools:

- `RoomHostBench` (no network, deterministic): the host's CPU cost of one committed room mutation in
  a 16-member room with four seated tables and a full 100-message chat history, the largest
  realistic snapshot. It also times the import every other member performs.
- `CustomRoomGameTest --perf` (real `sf4-net.exe` helpers): each member's tick in 2 to 16 member
  rooms. All helpers share one machine, so treat its numbers as relative; the helpers' own reported
  tick lag shows how contended the machine was.

## How to run

```
build\current\RoomHostBench.exe [--iterations N] [--legacy-clients]
build\current\CustomRoomGameTest.exe <abs path>\sf4-net.exe --perf --members 16 --per-table 4 --cycles 2 --frames 300 --json out.json
```

`--legacy-clients` makes the benchmark's members join like clients older than room chat deltas.

For the harness, `--members N --per-table P` lays members out in order, P per table; the first two
at a table fight and the rest spectate. `--single-table` still means one table. Other options:
`--game-cost-us`, `--relay-only`. Registered report-only CTest names (need `SF4E_NETWORK_TESTS`):
`RoomPerf.SingleMatch`, `TwoMatches`, `FourMatches`, `Spectators1`, `Spectators4`, `Spectators8`,
`SixteenFourTables`. Each member's tick is timed as its own slice, in the order the application runs
it. `match` is steady frames while GGPO runs; `lifecycle` is the control-plane tick during ready,
prepare, connect, result and terminal phases, the proxy for "another table rematches while I play".
The report's `helper` line is the worst helper-side load seen: actor tick lag and body time (the
2 ms tick shares the runtime's two workers with every gameplay bridge) and the fewest free slots in
the IPC event queue.

In the game, set `SF4E_ROLLBACK_DIAGNOSTICS=1`. A `FrameOver` log line attributes every outer tick
over 16.67 ms (rate limited to 4 per second), and the periodic summary lists the `room.*` ops.

## Where the cost was

Steady match frames cost under 2 ms of room work for every role. The cost is per committed room
mutation and it lands on every member: each member of a recovery-enabled room runs a passive
`SessionServer`, so every Ready, rematch, result, chat or acknowledgment at any table makes all 16
members receive and import a full checkpoint. That is how one table's activity disturbed another's
frame pacing.

## Results

`RoomHostBench`, one Ready or Unready at one table, p50 ms. Stages run on different ticks.

| Stage | Before | After |
|---|---|---|
| Host: step (journal and broadcast) | 78.2 | 6.3 |
| Host: propose | 10.3 | 5.5 |
| Host: apply commit | 6.8 | 1.5 |
| Every member: decode | on the game thread, twice | 11.1, on the worker |
| Every member: import | not measured separately | 1.1 |
| Live send per commit | 607 KB | 164 KB |

With `--legacy-clients` the step is 16.0 ms, since those clients still get the full chat in every
snapshot.

`CustomRoomGameTest --perf`, sixteen members, four tables, guest lifecycle tick p99 / max: 19.8 / 27
ms with 120 to 320 ticks over 16.67 before the decode worker, 4.5 / 8.4 ms and none over after it.

## Changes

1. `RoomRecoveryRuntime::Tick` decoded every commit a second time after `IrohRoom` had parsed,
   verified and compacted it (`BuildTerminalReplayEffects` a third time). The decoded form is handed
   over, and `RestoreRecoveryCheckpoint` takes it without a JSON round trip.
2. `CheckpointDecodeWorker` moves the remaining decode off the game thread. It sees only bytes and
   returns only values; commits are staged and imported in order. `SF4E_ROOM_WORKER=0` decodes inline.
3. Journaling a snapshot past the 256 KB candidate budget ran the whole-candidate shedding pass,
   which re-encodes every entry, once per remaining recipient. A projection now drops its own
   optional replay copy first; the pass stays for records that must be kept whole.
4. Each effect's payload is encoded once and reused for its digest, the journal size and the live
   send. Journal entry sizes are tracked, so compaction no longer re-encodes the whole journal.
5. A proposal is encoded directly, without a JSON copy of the checkpoint, and its effects once.
6. SHA-256 uses Windows CNG; the portable implementation remains as a fallback and test reference.
7. Room chat, two thirds of a snapshot in a busy room, is sent to a client only when its committed
   copy is stale. Clients advertise this with `roomChatDelta` at join; older clients keep getting the
   full chat. A successor after failover starts by sending everyone the full chat.
8. `pending_effects` and its digest were written into every checkpoint and never read. Removed.
9. Stale match acknowledgements (a spectator's `game_ready` after the fighters started) are dropped
   before they open a recovery candidate, which would build a full checkpoint for nothing.

Not pursued: per-table room deltas. Snapshots are still about 13 KB without chat, and every member
still decodes about 460 KB per commit, off the game thread. The host fits its budget in the
worst-case benchmark, so the protocol change is not warranted yet.

## Bugs found on the way

- Once a member who had chatted left, every later room snapshot failed client validation (a chat
  sender outside the roster), so every client dropped room control, and every follower rejected the
  replicated checkpoint. `Leave` now prunes the departed member's lines, and the reader drops such
  lines instead of rejecting the room, for state from older owners.
- After a leader handoff, committed effects replayed to a local client carry their original term and
  were rejected as stale, so the new leader's own client could miss the departure snapshot. Effects
  already in the activated journal are now delivered whatever their term.
- A Ready resent after a stale-table rejection went out under a new action id, so a caller waiting
  for its own id never got a reply. The reply now carries the caller's id, and the resend keeps the
  caller's input delay.

The first two caused the `IrohRecoveryIntegrationTest` failures seen before (2 of 3 runs in "normal
leader and moderator departure", including on the unmodified baseline).
