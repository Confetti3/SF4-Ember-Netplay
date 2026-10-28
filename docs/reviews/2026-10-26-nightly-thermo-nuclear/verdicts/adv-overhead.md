# Adversary verdicts: overhead.md (nightly b9ce0e31)

Read-only. Paths are relative to `C:\Users\Kate\Desktop\sf4\sf4-nightly` unless noted.

## Inventory checks (section 0 and the table)

- 9 s Raft heartbeats are empty: CONFIRMED. `coordination.rs:1124` sets `heartbeat_interval: 9_000`,
  `max_payload_entries: 1` (`:1133`). openraft 0.9.25 `replication/mod.rs:229-234` turns
  `Data::Heartbeat` into `LogIdRange(matching, matching)`, and `:392-395` sends `vec![]` when start == end.
  The core tick (`core/raft_core.rs:1292-1302`) sends it to every replication stream (followers and learners).
  CORRECTION to row B3: the per-commit heartbeat is real (`replication/mod.rs:671-685`, fed by
  `Command::ReplicateCommitted`, `raft_core.rs:1689-1691`) but the row's bytes only count the 9 s timer.
  At 0.5 commits/s and 15-16 followers that is another ~4-4.4 KB/s, so B3 is ~5 KB/s at 16 members, not 0.9.
  Still small next to A1.
- 1 s authority poll: CONFIRMED. `service/mod.rs:1382` (`interval(1 s)`) -> `:1500` `emit_coordination_state`
  -> `refresh.rs:144` `recovery.state()` -> `recovery.rs:407-437`: a follower runs `read_barrier`, which is an
  `authority` RPC to the leader (`coordination.rs:1200-1218`); the leader runs `authority_claim` with a 500 ms
  coalesce (2.5 s for the host, `coordination.rs:63-65,1239-1255`) and then `ensure_linearizable`
  (`raft_core.rs:284-344`, voters only, quorum of one returns at once, so B2 = 0 for a public host). Note
  `state()` also calls `committed.digest()` every second (`recovery.rs:410-411`); see F-OVH-2.
- "The large JSON every 15 s is the public rooms list": CONFIRMED as the only fixed-timer, whole-document
  JSON at 15 s. `rooms.js:9` `REFRESH_MS = 15000`, re-armed after each load (`:265-268`), stopped while hidden
  (`:276-280`), `cache: 'no-store'` (`:243`); `PublicRoomsPanel.hxx:40` `AutoRefreshSeconds = 15` (`.cxx:220,256`).
  Broad grep (`15000`, `15_000`, `from_secs(15)`, `= 15`, `15 s`, `15s`, long-period `constexpr` timers in
  `src/ui`, `src/netplay`, `src/session`, all `server/**/*.js`): every other 15 s hit is a timeout, a grace
  period, a cache TTL or a log window. Notable ones that are not periodic sends: `PUBLIC_BOARD_SECS = 15` and
  the `/rankings.json` nginx `proxy_cache_valid 15s` (`ranking.rs:72`, `ember-short-locations.conf:125`) are
  cache lifetimes; the page itself polls every 30 s (`rankings.js:11`), so nginx refetches the <= 22 KB board
  from the bridge at most every 15 s over loopback while someone is viewing. `kNetplayWindowMs`
  (`sf4e__Game__Battle__System__Ggpo.cxx:17,69,604`) is one small log line. The 15 s checkpoint loops
  (`IrohRoomCheckpoint.cxx:296-304`, `CHECKPOINT_TRANSFER_TIMEOUT`) only fire while a room is unhealthy.
  Not in the tree: the BluMint match board (`board.py` on the VPS, per memory note
  blumint-match-board-2026-10-23) rewrites `status.json` every 10 s; its page's poll rate is not in the repo.
  It runs at a different cadence and location, so it is not the 15 s item, but it is the one other timed JSON.
- Bytes/s arithmetic: internally consistent. P = 460 KB -> 460 x 1.15 / 8 x 4/3 = 88 KB; x 15 followers x
  0.5/s = 660 KB/s; public x 16 = 704 KB/s; pipe 460 x 4/3 = 613 KB x 0.5 = 306 KB/s; A3 164 KB x 0.5 = 82 KB/s
  (164 KB is the baseline doc's measured live send). The struct shapes back the sizes (`AuthorityClaim` 4 u64 +
  two u64 sets ~550 B; `CompressedJsonWire.digest` is a 32-number array). The weak inputs are the 8x ratio and
  r = 0.5/s, both unmeasured. One bias to state: deflate does better than 8x on the superseded projections
  (near-duplicates of each other inside the 32 KB window), so F-OVH-1's compressed Raft saving is overstated,
  while its uncompressed savings (pipe, parse, decode, SHA) are as stated.

## Findings

### F-OVH-1: PLAUSIBLE  (severity: keep P1)
- Decisive invariant/evidence: the mechanism is right. `CompactEffectJournal` drops superseded projections only
  inside the over-budget loop (`SessionRecovery.hxx:210-217`), `ApplyCommit` appends every candidate effect to
  `_committedEffectHistory` (`sf4e__SessionServer__Recovery.cxx:349-361`), and `RecoveryCheckpoint()` writes the
  whole history (`sf4e__SessionServer__RecoveryCheckpoint.cxx:290`), so a 2-member room's journal is ~250 KB of
  full-payload superseded snapshots. It is not a wire or Raft shape change: the readers bound count and bytes
  but never require a minimum (`RecoveryCheckpoint.cxx:296-319`), `effectsDigest` covers only
  `proposal.effects` (`SessionRecovery.hxx:306-309`), the checkpoint is an opaque string to Raft, nothing in
  Rust reads `effect_journal`, `BuildTerminalReplayEffects` reads `room.terminal_receipts`, not the journal
  (`IrohRoomCheckpoint.cxx:149-152`), and old learners behind a newer public host accept a shorter journal.
  The baseline (`_recoveryBaseline`) is captured in `BeginRecoveryCandidate` before any candidate effect exists
  (`Recovery.cxx:39`), so part (2) is safe for the no-op check and the term-loss restore.
  What the finding misses: the journal is not only a replay store. It is the authorization record for live
  effects that a member evaluates after a later commit has activated. `AuthorizedEffect` accepts a `_commit`
  token only if it is in `appliedEffects_` (`IrohRoom.cxx:591-593`); a token whose revision is now <= the
  activated revision and not found is silently dropped (`:597-605`, `IrohRoomAdapters.cxx:109-113`). Live messages
  routinely arrive before their commit and park (`return 0`, `:604`); up to four commits can be staged
  (`IrohRoom.cxx:519`) and `RoomRecoveryRuntime::Tick` activates all staged heads in one loop
  (`RoomRecoveryRuntime.hxx:20-125`). Today a parked commit-k `data_update` is still found in commit k+1's
  compacted journal in small rooms, because the budget pass removes the oldest superseded entries first and
  k's are the newest. Erasing every superseded entry turns that into a drop. For `room_snapshot` the end state
  is the same (full state, revision-gated, `sf4e__SessionClient__Room.cxx:395-397`). For `data_update` it is
  not guaranteed: a grant takes the projection matching its generation (`sf4e__SessionClient__Room.cxx:162-180`)
  and then requires the roster to match it exactly (`IrohMatchSession.cxx:164-178`, `stale_match_roster`), so a
  later same-generation projection with a changed spectator list can fail a match start that today succeeds.
  The same race already exists today in 16-member rooms (decode evicts about half of commit k's projections
  there), so the proposal widens an existing edge to every room size.
- Adjustment: do not erase superseded projections outright. Keep their envelopes and drop only their
  `publicPayload` (digest-only), which leaves authorization unchanged because `EffectEnvelope::operator==`
  ignores the payload (`SessionRecovery.hxx:81-85`, and the existing test at `session_recovery_gate_test.cxx:59-69`
  asserts exactly this identity rule). Then erase a superseded digest-only entry only once its revision is more
  than 4 behind the newest revision, so the journal stays small. "Newest" must be the maximum `revision` among
  the entries being compacted, not the owner's authority revision, so the leader (history + candidate) and a
  follower (journal + effects) compute the same set. Staging parks at 4 staged+decoding (`IrohRoom.cxx:519`), so a
  parked token is at most 3 revisions behind the newest activated head; 4 is a deliberate one-commit margin. Keep the chat exception
  (the newest chat-bearing snapshot keeps its payload while a later one says `chat_unchanged`). Apply the rule in
  `CompactEffectJournal` so leader and decode stay identical (`session_server_transport_test.cxx:1088-1112`).
  Restate savings: P at 2 members ~50-60 KB rather than 35 KB (256 digest-only entries at ~350 B would be
  ~90 KB, hence the revision window); the compressed Raft saving is smaller than the table says; the pipe,
  parse, decode and SHA savings stand. Add a test where commits k and k+1 activate in one Tick with k's live
  `data_update` parked.
- Duplicate of: no (canonical; F-OVH-2, -3 and -4 savings all scale with it)

### F-OVH-2: CONFIRMED  (severity: P2 before F-OVH-1, P3 after)
- Decisive invariant/evidence: `seats.rs:180` and `latency_roster.rs:73-74` each build a `serde_json::Value`
  of the whole checkpoint (cached per commit), `coordination.rs:640` builds one per probe. Helper-internal.
- Adjustment: drop the "plus one SHA-256" from the saving. `RecoverySession::state()` calls `committed.digest()`
  every second for `applied_digest` (`recovery.rs:410-411`) and the digest is a `OnceLock` (`coordination.rs:219-258`),
  so it is computed once per commit regardless of the latency cache key. The typed view must keep
  `committed_latency_roster`'s exact row rules (frozen rows, duplicate and primary-equal latency IDs dropped).
- Duplicate of: no

### F-OVH-3: CONFIRMED  (severity: keep P2)
- Decisive invariant/evidence: `Peer::append_entries` encodes per follower (`coordination.rs:1041-1053`), then
  `Peer::request` serializes the wrapper again (`:1031`); `try_get_log_entries` clones per send (`:716-728`).
  For in-sync followers OpenRaft builds the same `vote`, `prev_log_id`, one entry (`max_payload_entries: 1`) and
  usually the same `leader_commit`. zlib output is deterministic, so a memo keyed as proposed yields byte-identical
  bodies. A `leader_commit` mismatch just misses.
- Adjustment: replication streams run concurrently, so a plain check-then-store memo lets every stream miss on the
  first commit; encode under the slot lock (or a per-key once-cell). Keep the RawValue step out: the log entry JSON
  is replicated state and snapshot content (`Machine`), so it needs the capability fence the finding names. CPU
  saving shrinks in proportion to P after F-OVH-1; the VPS host of a full public room is where it still matters.
- Duplicate of: no

### F-OVH-4: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: 16 KiB chunks, window 4 (`recovery.rs:24-25`), base64 in JSON, 8 events per poll
  (`IrohRoom.cxx:794`), JSON `checkpoint_ack` per chunk. Helper and game ship together; the IPC frame carries
  `VERSION = 1` and `read_frame` rejects anything else (`wire.rs:10,47-49`), so a binary kind must be signalled
  inside the payload or by a version both ends accept.
- Adjustment: after F-OVH-1, P in small rooms is 3-4 chunks, so this is mostly subsumed; do it only for large
  rooms, and prefer the binary frame over bigger JSON chunks (the finding is right that bigger chunks only move
  the parse into bigger spikes).
- Duplicate of: no (partly subsumed by F-OVH-1)

### F-OVH-5: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: `AuthorizedEffect` parses and re-dumps (`IrohRoom.cxx:548,611`), the adapter parses
  again for `type` (`IrohRoomAdapters.cxx:115-117`), activation parses every queued message for
  `_terminal_replay` (`IrohRoomCheckpoint.cxx:245-260`); `Checkpoint()`/`RestoreCheckpoint` dump only to measure
  (`RecoveryCheckpoint.cxx:59,66`), `:297` likewise. Pipe-only, same install.
- Adjustment: none. The digest at `IrohRoom.cxx:586` must stay computed on the object with `_commit` erased, as now.
- Duplicate of: no

### F-OVH-6: PLAUSIBLE  (severity: keep P2; it is the owner's literal ask)
- Decisive invariant/evidence: no `gzip_types` for JSON (`ember-short-locations.conf:173` is `/assets/` only),
  `cache: 'no-store'`, bridge recomputes per request (`rooms/mod.rs:423-466`).
- Adjustment: (a) the in-game path gains nothing from nginx gzip as written: the helper builds reqwest with
  `default-features = false, features = ["rustls-no-provider"]` (`rust/sf4-net/Cargo.toml:29`), so it sends no
  `Accept-Encoding: gzip`; enable reqwest's `gzip` feature too (same install, no compat issue). (b) `listed_at`
  is `state.now()` per request (`rooms/mod.rs:439,445,509`), so a body ETag never matches; hash `rooms` only and
  on 304 have the page and the panel set their own "listed" time (`rooms.js:253` reads `body.listed_at`).
  (c) Whether the VPS has http-level gzip is not in the repo. The rooms data is public and carries no secret,
  so compression has no BREACH-style concern.
- Duplicate of: no (canonical for section 0, D1, D2, D3)

### F-OVH-7: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: 1 s poll and 500 ms coalesce as described (`coordination.rs:63,1239-1255`).
- Adjustment: (a) is a bounded timing change, not free: the leader may report `writable` from a claim up to 1.2 s
  old instead of 0.5 s, still under `AUTHORITY_READ_TIMEOUT` = 2 s (`recovery.rs:30-33`); Raft commit stays
  quorum-gated, so only the room-pause latency moves. State it that way. (b) changes quorum-loss detection inputs
  for public-room learners and only helps while commits flow (heartbeats are 9 s, so a quiet room still needs the
  RPC); defer. Saving is ~2-12 KB/s; do last.
- Duplicate of: no

### F-OVH-8: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: `poll` writes every reported room and calls `state.committed()` on any successful
  transaction (`poll.rs:92-98,150-186`); `committed()` notifies event waiters, delivery and the stream signal
  (`lib.rs:82-87`).
- Adjustment: confirm nothing uses the 5 s wake as a timer (SSE lease checks, delivery retry pacing) before gating
  `committed()` on "wrote an event"; the maintenance calls (`routes/mod.rs:194,205,251`) may already cover that. The
  per-room digest is in memory, so a bridge restart writes every room once, which is fine.
- Duplicate of: no

### F-OVH-9: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: `HelperClient.cxx:144-181` loops on `PeekNamedPipe` + 2 ms waits; outgoing pushes
  do not wake it. `pump_committed_checkpoint` reads the store every tick (`checkpoints.rs:265-275`).
- Adjustment: `pump_committed_checkpoint` returns early while a transfer is in flight (`checkpoints.rs:275-280`),
  so a `committed_changes()` arm alone would miss the commit that landed during a transfer; also re-check when a
  transfer finishes. Overlapped I/O needs the pipe opened with `FILE_FLAG_OVERLAPPED` on both read paths.
- Duplicate of: no

### F-OVH-10: REJECTED  (severity: P3)
- Decisive invariant/evidence: real (`controls.rs:308-370`), but publications happen only on a new leader or a
  retirement; N-2 serializations of ~8 KB per such event is microseconds.
- Adjustment: none; not worth a change.
- Duplicate of: no

## Batch verdict

- Counts: CONFIRMED 3 (F-OVH-2, -3, -5), PLAUSIBLE 6 (F-OVH-1, -4, -6, -7, -8, -9), REJECTED 1 (F-OVH-10).
  Inventory: all three asked claims hold; B3 understated (~5 KB/s, not 0.9, at 16 members).
- Do first:
  1. F-OVH-1, digest-only variant with a revision window. It is the dominant cost in every room size and needs no
     wire, Raft or snapshot shape change; the adjustment keeps live-effect authorization identical.
  2. F-OVH-6 web half (gzip on `/rooms.json` and `/rankings.json`, cache revalidation). It is the owner's literal
     "15 s JSON", a config-only change with no client compatibility issue; the in-game half needs reqwest `gzip`.
  3. F-OVH-3. Byte-identical memo, biggest CPU item on the VPS host of a busy public room.
- Could not verify by reading: real compression ratio and commit rates (no run allowed); whether any UI
  `OnRoomSnapshot` consumer diffs consecutive snapshots (a dropped intermediate projection would merge two
  changes); VPS http-level nginx gzip; the match board page's poll cadence (VPS only); the details of
  `Store::committed_changes` beyond its existence (`coordination.rs:499`).
