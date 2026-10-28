# Nightly code quality and overhead review, 2026-10-26

Scope: the whole tree of sf4-nightly, branch `nightly`, head b9ce0e31. No diff; every area was read as it stands.

## 1. Method and counts

Twelve area reviews each took one area and wrote findings in a fixed schema (`findings/*.md` under `docs/reviews/2026-10-26-nightly-thermo-nuclear/`). Eight adversarial verification passes then opened every cited line and ruled on every finding: CONFIRMED, PLAUSIBLE with a stated adjustment, or REJECTED. They also corrected severities, corrected estimates and marked duplicates (`verdicts/adv-*.md`). Everyone worked read-only. Nothing was built, tested or run, apart from one read-only timing of the source fingerprint script, so no other number in this report is a measurement. Every byte, millisecond and allocation figure was derived by reading code and struct shapes, and the commit rates and the 8x compression ratio behind the Raft numbers are assumptions. Read "about" in front of every figure.

Where a finding and its verdict disagree, this report follows the adversary. Each work item carries the adversary's adjustment, not the reviewer's first proposal.

The brief for this report said 184 findings. The input files hold 179 finding blocks and 179 verdict blocks; the index in section 7 has 179 rows.

| Area | File | Findings | Confirmed | Plausible | Rejected | Split ruling | Duplicate verdict | Final P1 | Final P2 | Final P3 |
|---|---|---|---|---|---|---|---|---|---|---|
| Overhead inventory (F-OVH) | overhead.md | 10 | 3 | 6 | 1 | 0 | 0 | 1 | 3 | 6 |
| Rust coordination (F-COORD) | rust-coordination.md | 15 | 7 | 4 | 3 | 0 | 1 | 1 | 4 | 10 |
| Rust helper actor (F-SVC) | rust-service.md | 14 | 7 | 6 | 1 | 0 | 0 | 0 | 2 | 12 |
| Game session and pipe (F-SESSION) | game-session.md | 15 | 10 | 4 | 1 | 0 | 0 | 2 | 3 | 10 |
| Game hooks and runtime (F-SF4E) | game-sf4e.md | 16 | 4 | 11 | 1 | 0 | 0 | 0 | 9 | 7 |
| Game UI, launcher, Discord (F-UI) | game-ui.md | 18 | 3 | 12 | 3 | 0 | 0 | 0 | 1 | 17 |
| Bridge (F-BRIDGE) | server-bridge.md | 16 | 5 | 11 | 0 | 0 | 0 | 1 | 1 | 14 |
| Rooms, room host, site (F-ROOMS) | server-rooms.md | 10 | 3 | 7 | 0 | 0 | 0 | 0 | 2 | 8 |
| Protocol, brackets, notifier (F-PROTO) | server-protocol.md | 15 | 8 | 5 | 1 | 1 | 0 | 0 | 3 | 12 |
| Helper tournament and IPC (F-TOUR) | rust-tournament.md | 14 | 8 | 5 | 1 | 0 | 0 | 0 | 1 | 13 |
| Tests (F-TEST) | tests.md | 14 | 9 | 5 | 0 | 0 | 0 | 0 | 5 | 9 |
| Tooling, SDK, deploy (F-TOOLS) | tools.md | 22 | 9 | 11 | 1 | 1 | 0 | 0 | 4 | 18 |
| Total | | 179 | 76 | 87 | 13 | 2 | 1 | 5 | 38 | 136 |

"Split ruling" means part rejected and part plausible (F-PROTO-8, F-TOOLS-22). Only F-COORD-5 carried DUPLICATE as its verdict. Beyond that, 22 findings are merged, fully or partly, into another finding's work item as duplicates, and five more are marked as part of a larger item; the index names the canonical one. Reviewers marked 17 findings P1. The adversaries kept five: F-OVH-1, F-COORD-1 (for public room hosts), F-SESSION-1, F-SESSION-2 and F-BRIDGE-1. Most structural P1s dropped to P2 or P3 because they bring no overhead win.

## 2. The large JSON every 15 seconds

The JSON fetched whole on a fixed 15 s timer is the public rooms list. It is fetched in two places.

- Website: `server/ember-short/deploy/site/assets/rooms.js:9` sets `REFRESH_MS = 15000`, re-arms after each load (`:265-268`), stops while the tab is hidden and fetches with `cache: 'no-store'` (`:243`). nginx serves `/rooms.json` from a 5 s cache (`ember-short-locations.conf:83-88`) in front of the bridge's `GET /v1/rooms/public` (`server/ember/bridge/src/routes/rooms/mod.rs:506`).
- In game: `src/ui/PublicRoomsPanel.hxx:40` sets `AutoRefreshSeconds = 15`. It only runs while the Public rooms screen is drawn, the player is idle and the previous answer is in. The helper calls `GET /v1/rooms?build_id=...&detail=1` (`rust/sf4-net/src/tournament/rooms.rs:95-130`), turns the list into a `serde_json::Value`, and sends it over the pipe as `Event::Tournament`, where nlohmann parses it again.

Size: up to 100 `RoomSummary` rows (`server/ember/protocol/src/rooms.rs:202-239`). The UI adversary corrected the per-room size to about 280-380 B because `build_id` (up to 128 chars) and the `detail=1` fields are included. That is about 13-17 KB at the VPS's 45-room capacity and up to about 35 KB at the 100-row limit, so about 1 KB/s per open tab or per in-game viewer. The rankings board (`/rankings.json`, up to 500 entries, about 22-40 KB) is polled every 30 s by `rankings.js:11`.

What it is not:

- The room checkpoint (about 280-460 KB) is the large JSON in Ember, but it is sent per committed room mutation, not on a timer. Its only 15 s loops are failure paths: a proposal not committed after 15 s is abandoned (`src/session/IrohRoomCheckpoint.cxx:296-304`) and retried, and the helper's export retries after `CHECKPOINT_TRANSFER_TIMEOUT` = 15 s (`rust/sf4-net/src/service/mod.rs:74`). A large JSON seen every 15 s in a log while a room was unhealthy would be that loop.
- The Raft heartbeat every 9 s is empty: `heartbeat_interval: 9_000` (`coordination.rs:1124`), and openraft sends `entries: []` for a heartbeat (verified in openraft 0.9.25 `replication/mod.rs:229-234, 392-395`).
- Every other 15 s constant is a timeout, a grace period, a cache lifetime or a log window: the latency `LEASE`, iroh's `PATH_MAX_IDLE_TIMEOUT`, `DepartureGraceMs`, the `/rankings.json` 15 s cache, `PUBLIC_BOARD_SECS`, and `kNetplayWindowMs` (one log line per 15 s, `sf4e__Game__Battle__System__Ggpo.cxx:69`).
- The BluMint match board's `status.json` on the VPS is rewritten every 10 s, not 15 s. It is not in the tree and its page poll rate is unknown.

What fixes it (work item T1-9):

- nginx `gzip on; gzip_types application/json; gzip_vary on;` on `/rooms.json` and `/rankings.json`. In the tree the only `gzip_types` is the `/assets/` block (`ember-short-locations.conf:173`), so both JSON answers go out uncompressed unless the live nginx.conf adds JSON. The lines are idempotent if it does. Expected cut is about 75-85% (estimate).
- For the in-game list, nginx gzip alone does nothing. The helper builds reqwest with `default-features = false, features = ["rustls-no-provider"]` (`rust/sf4-net/Cargo.toml:29`), so it never sends `Accept-Encoding: gzip`. The fix needs reqwest's `gzip` feature plus gzip for `/v1/rooms` on the bridge vhost. I checked: `server/ember/bridge/deploy/nginx/bridge.conf.in` has no gzip and the bridge has no compression layer.

What does not fix it:

- A whole-body ETag never matches: `listed_at` is `state.now()` on every request (`rooms/mod.rs:439,445,509`). An ETag would have to hash `rooms` only, and the page and panel would set their own "listed" time on a 304.
- An in-game "unchanged" answer is a new answer shape. `RoomList` is `deny_unknown_fields` in every released helper, so it would need a distinct status sent only when the request asked for it. The UI adversary judged the saving (about 1 KB/s per viewer, only while the screen is open) not worth a protocol change now.
- Caching the serialized body inside the bridge was rejected. nginx already caps bridge hits at one per 5 s, and the cache would move `listed_at` from request time to poll time.

## 3. Where the overhead really is

The periodic plane is lean. The cost is per commit, and it follows one chain. Commit-rate assumptions used throughout: r = 0.07 commits/s (2 members), 0.25/s (8), 0.5/s (16). A quiet room commits nothing.

### 3.1 The room checkpoint pipeline, end to end

1. Effect journal. `CompactEffectJournal` (`src/session/SessionRecovery.hxx:210-217`) drops superseded `room_snapshot` and `data_update` projections only inside its over-budget loop. Every commit's `BroadcastRoomState` journals a fresh projection per member, so the journal sits at its 256 KB / 256-entry budget in every room size, two members included. The encoded proposal P is therefore about 280 KB (2 members), 320 KB (8) and 460 KB (16), most of it dead projections that the same proposal already supersedes (F-OVH-1). The adversary found the hazard the reviewer missed: the journal is also the authorization record for live effects. `AuthorizedEffect` accepts a `_commit` token only if it is in `appliedEffects_` (`IrohRoom.cxx:591-605`), up to four staged commits activate in one Tick, and a parked commit-k `data_update` that is erased from commit k+1's journal is silently dropped. A later same-generation projection with a changed spectator list can then fail a match start with `stale_match_roster`. So the fix keeps superseded envelopes and drops only their payload (digest-only), and erases a digest-only entry only once it is more than 4 revisions behind the newest. That keeps authorization identical because `EffectEnvelope::operator==` ignores the payload. Restated saving: P at 2 members falls to about 50-60 KB, not the reviewer's 35 KB.
2. Leader encode. `Peer::append_entries` runs `CompressedJsonWire::encode` once per follower (`coordination.rs:1041-1053`): JSON of the request re-escaping the whole checkpoint string, zlib, blake3, base64, then `Peer::request` serializes the wrapper again, from a fresh clone of the entry per send (`:716-728`). At 16 followers that is 15 or 16 identical encodes per commit, estimated 40-75 ms of CPU on the runtime workers that also carry gameplay bridges (F-OVH-3, F-COORD-1). The checkpoint `String` is also copied four to six times per commit per node (F-COORD-3). The adjusted fix is a shared, encode-once memo keyed by `(vote, prev_log_id, leader_commit, entry LogIds)` under a slot lock, plus `Arc<str>` for the checkpoint. serde's `rc` feature is already on, so `Arc<str>` serializes exactly like `String` and nothing on the wire or in the snapshot changes. Compress-once at propose time cannot stand alone: JSON writes `Arc<[u8]>` as a number array three to four times larger, so it needs the binary codec of F-COORD-2, a `SNAPSHOT_MAGIC` bump and a helper wire version in the compared build identity (T2-3).
3. Every member's helper. `observe_commit` (`service/seats.rs:180`) and `committed_latency_roster` (`coordination/latency_roster.rs:74`) each build a full `serde_json::Value` tree of the 460 KB checkpoint to read a few fields, and every probe builds a third (F-SVC-4, F-COORD-5, F-OVH-2). Separately, the pump copies the checkpoint (`checkpoints.rs:300`) and `CheckpointTransfer::new` runs a second SHA-256 over the copy (`recovery.rs:759`) although `Committed::digest()` already caches the same SHA-256 of the same bytes in a `OnceLock` (`coordination.rs:252`) and `state()` computes it every second. The overhead adversary was right that the latency cache key's digest costs nothing extra; the core adversary was right that the transfer's digest does. I confirmed both in source.
4. Pipe. The helper exports each commit as about 30 `checkpoint_chunk` events of 16 KiB, base64 inside JSON, window 4, each acknowledged by a JSON `checkpoint_ack` (F-SVC-14, F-OVH-4, F-SESSION-5). A binary frame is possible because the frame layer is length-prefixed, but the frame header's u16 is the shared `wire::VERSION` that also heads QUIC control frames and game UDP packets. It must never be bumped for an IPC change; fork `read_ipc`/`write_ipc` and bump an IPC-only version in the bootstrap instead. The byte saving does not shorten the transfer (credit is counted in raw bytes); the gain is game-thread CPU, and most of the base64 cost goes away with a table decoder and no protocol change (F-SESSION-3).
5. Game thread. Each chunk is base64-decoded with a linear `alphabet.find` per character after a per-call heap allocation of the alphabet, and copied four times (F-SESSION-3, about 0.5-1.5 ms per tick across a credit window). A committed effect is parsed four times, dumped twice and hashed once before it reaches `SessionClient` (F-SESSION-1). A parked effect, the normal case while its checkpoint is still crossing the pipe, is re-parsed, re-dumped and re-hashed on every poll for about 5-10 ticks (F-SESSION-2). Two rules decide every fix here. The digest must stay SHA-256 over `dump()` of the parsed DOM with `_commit` erased, never over a substring of the received text. The digest must still be checked on first sight, so a forged or corrupt token with a future term or revision is dropped at once instead of parking the queue.

Estimated bytes per second per busy room, from the inventory in `findings/overhead.md` with the adversary's corrections:

| Stream | 2 members | 8 members | 16 members |
|---|---|---|---|
| A1 Raft append fan-out, private room | about 3.7 KB/s | about 107 KB/s | about 660 KB/s |
| A1 Raft append fan-out, public room (VPS egress) | about 7.4 KB/s | about 122 KB/s | about 704 KB/s |
| A2 Pipe export, per member, local | about 26 KB/s | about 107 KB/s | about 306 KB/s |
| A3 Live snapshots, leader to members | about 0.8 KB/s | about 21 KB/s | about 82 KB/s |
| B1 Authority reads, private / public | about 0.7 / 1.4 KB/s | about 4.5 / 5.2 KB/s | about 11 / 12 KB/s |
| B2 Leader barriers (public room: 0) | 0 | about 4.4 KB/s | about 4.4 KB/s |
| B3 Raft heartbeats, 9 s timer plus one per commit (corrected) | about 0.1 KB/s | about 1.4 KB/s | about 5 KB/s |
| B4 QUIC keepalives with the latency board open | about 0.1 KB/s | about 1.7 KB/s | about 6.5 KB/s |

A1 assumes 8x compression and the commit rates above, both unmeasured. The adversary's correction to B3 adds the heartbeat openraft sends right after each commit to carry `leader_commit`; the inventory counted only the 9 s timer. Deflate likely does better than 8x on near-duplicate projections, so the compressed Raft saving of T1-1 is smaller than the uncompressed savings (pipe, parse, decode, SHA), which stand.

### 3.2 The server side

- Bridge rooms poll (F-BRIDGE-1 canonical, F-OVH-8, F-ROOMS-1). Every 5 s the bridge fetches the supervisor's full room list and, for every reported room, re-seals the invitation with a fresh random nonce (`secrets.rs:110-118`), runs an unconditional `UPDATE`, loads the row twice, and calls `state.committed()`, which wakes every SSE stream and a delivery worker. The new ciphertext guarantees a dirty page, so under WAL with `synchronous=FULL` (`db.rs:69-70`) each poll is one fsync while at least one room is reported (none with zero rooms). That is about 17,280 fsyncs a day. Nothing depends on the 5 s wake as a heartbeat. F-ROOMS-1's supervisor change counter is a follow-on and has a race the reviewer missed: the supervisor reports a room as soon as the host says `hosted`, before the bridge's create path stores `invitation_sealed`. A poll in that window matches zero rows, and a cached counter would then skip the room's host-reported fields until the room changes again. The counter may be cached only after an UPDATE that matched a row and committed.
- Delivery workers (F-BRIDGE-3). Four workers share one `Notify`, and `committed()` calls `notify_one`, so a commit wakes one worker by luck and the rest find work on a 1 s timeout. Each idle worker runs a `BEGIN IMMEDIATE` lease every second: four mutex holds per second. The adversary showed these dirty no page on an idle bridge, so there is no fsync.
- Notifier inbox (F-PROTO-1). The notifier runs its due query every second over an `inbox` table with no index on the filtered columns and no pruning, so each pass reads every stored event body ever received.
- Room host (F-ROOMS-2). `RoomHost::Quiet()` requires an empty room, so any occupied room ticks every 16 ms (about 62 Hz) even when players only sit in the lobby. Player actions arrive as helper events that end the wait at once, so a longer idle wait adds no delay to them. Timer transitions (ready 90 s, spectator hold 10 s, result dispute 30 s) would fire up to 250 ms late with a plain 250 ms cap, and on time with a `NextDueMs` deadline. The CPU saving (about 0.2-0.3% of a core per idle occupied room, about 10% across 45 rooms) is an assumption to measure with the soak monitor first.

## 4. Work plan

Each task block is meant to be picked up alone. "Keep identical" names the invariant that decides whether the change is safe. Test names are CTest names unless marked as cargo modules; HelperRust and EmberRust are the cargo suites registered in CTest.

### Tier 1: do first

#### T1-1 Effect journal: keep superseded projections digest-only (P1)
- Ids: F-OVH-1.
- Files: `src/session/SessionRecovery.hxx:197-250` (`CompactEffectJournal`), `src/session/sf4e__SessionServer__Recovery.cxx:348-361`, `src/session/sf4e__SessionServer__RecoveryCheckpoint.cxx:289-290`, `src/session/CheckpointDecodeWorker.hxx:55-61`.
- Change: in `CompactEffectJournal`, before the budget loop, turn every `room_snapshot` or `data_update` that has a later entry for the same `(recipient, type)` into a digest-only entry: keep the envelope, drop `publicPayload`. Erase a superseded digest-only entry only when its revision is more than 4 behind the newest revision among the entries being compacted. "Newest" is the maximum `revision` in the set, not the owner's authority revision, so the leader (history plus candidate) and a follower (journal plus effects) compute the same set. Keep the chat exception: the newest chat-bearing snapshot keeps its payload while a later one says `chat_unchanged`. Apply the rule only in `CompactEffectJournal`. Do not erase superseded projections outright, as the reviewer proposed.
- Keep identical: `EffectEnvelope::operator==` ignores the payload (`SessionRecovery.hxx:81-85`), so live-effect authorization in `AuthorizedEffect` is unchanged; staging parks at 4 (`IrohRoom.cxx:519`), so a parked token is at most 3 revisions behind. `effectsDigest` covers only `proposal.effects`. The checkpoint is an opaque string to Raft, so there is no Raft, snapshot or wire change, and old learners behind a newer public room host accept a shorter journal.
- Verify: SessionRecoveryGate (its test at `session_recovery_gate_test.cxx:59-69` asserts the identity rule), CheckpointDecodeWorker, SessionServerTransport (leader and decode compaction must match, `session_server_transport_test.cxx:1088-1112`), IrohRecoveryIntegration. Add a test where commits k and k+1 activate in one `RoomRecoveryRuntime::Tick` with k's live `data_update` parked. Have RoomHostBench print the encoded proposal size and run RoomPerf.* before and after. In game: an 8+ member public room on a real room host for a session, watching for `stale_match_roster`. Not verified by reading: whether any `OnRoomSnapshot` consumer diffs consecutive snapshots.
- Saving: P at 2 members from about 280 KB to about 50-60 KB. The reviewer's 8 and 16 member figures (about 120 / 280 KB) were for the erase variant; expect somewhat less. Pipe, decode and hash savings scale with P on every member; the compressed Raft saving is smaller.

#### T1-2 Encode each Raft append once and share the checkpoint bytes (P1 on public room hosts, P2 in private rooms)
- Ids: F-OVH-3, F-COORD-1 (memo step only), F-COORD-3, F-SVC-14 (copy half).
- Files: `rust/sf4-net/src/coordination.rs` (`Network`, `Peer::append_entries` :1041-1053, `Peer::request` :1031, `try_get_log_entries` :716-728, apply :884, `build_snapshot` :740, `get_current_snapshot` :985), `rust/sf4-net/src/recovery.rs:593,759`, `rust/sf4-net/src/service/checkpoints.rs:290-300`.
- Change: (1) one memo in `Network`, shared by every `Peer`, keyed by `(vote, prev_log_id, leader_commit, first and last entry LogId)` and holding the final RPC body as `Arc<Vec<u8>>`, bounded to the last one or two requests. Encode under the slot lock or a per-key once-cell: replication streams run concurrently, and a plain check-then-store lets every stream miss on the first commit. (2) `Proposal.checkpoint: Arc<str>`, shared by `Committed`; `CheckpointTransfer.bytes` as `Arc<[u8]>` or `bytes::Bytes`; `recovery::propose` builds from the transfer's buffer. (3) Pass `Committed::digest()` into `CheckpointTransfer` instead of hashing again at `recovery.rs:759`. Leave out the `RawValue` step; it changes the log entry JSON.
- Keep identical: RPC bodies stay byte-identical (zlib output is deterministic; a `leader_commit` mismatch just misses). serde `rc` (Cargo.toml:31) makes `Arc<str>` serialize as a string, so the Raft entry shape, append bytes and snapshot bytes do not change and `SNAPSHOT_MAGIC` stays. `same_commit` keeps working.
- Verify: HelperRust (cargo modules `coordination` and `recovery`), IrohRecoveryIntegration, IrohRoomIntegration, RoomPerf.*. Add a test that two in-step followers receive identical bodies from one encode. In game: a busy public room on the VPS host, comparing helper CPU with `soak-monitor.sh`.
- Saving: about 94% of per-commit append encode CPU at 16 followers (reviewer estimate 40-75 ms per commit, unmeasured); one 460 KB SHA-256 and one copy per commit on every member's actor; N-1+3 copies on the leader. Bytes unchanged.

#### T1-3 One typed view of the committed checkpoint per commit (P2)
- Ids: F-SVC-4 (canonical), F-OVH-2, F-COORD-5.
- Files: `rust/sf4-net/src/service/seats.rs:180`, `rust/sf4-net/src/coordination/latency_roster.rs:73-104`, `rust/sf4-net/src/coordination.rs:617-714`, `rust/sf4-net/src/service/latency/authority.rs:204-220`, `rust/sf4-net/src/service/mod.rs:554-622`.
- Change: add `Committed::view()` as a `OnceLock` beside `digest`, deserializing only `checkpoint.members[]` and `checkpoint.room.snapshot.{members, tables, tournament}` and skipping the rest. `observe_commit`, `committed_latency_roster` and `committed_probe_pair_bound` read it. Reproduce today's `Value` leniency exactly: `as_i64` for table ids and `as_u64` for member ids; absent or wrong type means `None` (a small `deserialize_with`); both endpoint key spellings; last-wins on duplicate keys, where a derive would error; `Cow<str>`, not `&str`; every input that yields `RosterInvalid` today still does. Keep `Seats::read` on a small `Value` subtree and keep the roster's row rules (frozen rows, duplicate and primary-equal latency ids dropped). Key the latency cache on `(revision, term)` plus `same_commit`.
- Keep identical: helper-internal; no pipe or wire change.
- Verify: HelperRust (`coordination::latency_roster`, `service::seats`, probe tests). Add one test per alias, per wrong-type leaf and for a duplicate key.
- Saving: about 1-2 ms per avoided full parse of about 460 KB (adversary figure, not the reviewer's 3-6 ms), two to three parses per commit per member, on the actor. Smaller after T1-1.

#### T1-4 Build the Raft snapshot outside the store lock; pump from a cached revision (P2)
- Ids: F-COORD-4, F-SVC-7.
- Files: `rust/sf4-net/src/coordination.rs:492,731-745`, `rust/sf4-net/src/service/checkpoints.rs:265-280,391-393`, `rust/sf4-net/src/service/refresh.rs:95-103`.
- Change: `build_snapshot` clones `memory.machine` under the lock (cheap, `committed` is an `Arc`), releases it, encodes, and re-locks only to store, and stores only if its `last_log_id` is at least that of the snapshot already stored, so an `install_snapshot` landing in between is not overwritten. For the pump, the forwarding task in `install_recovery` writes the latest revision to an `Arc<AtomicU64>` before it notifies `wake`; `pump_committed_checkpoint` returns early while that is at most `last_exported_revision`. Compare with the cached revision, not `has_changed()`, because the timeout path lowers `last_exported_revision` with no watch change. Re-check when a transfer finishes, since the pump returns early during one.
- Keep identical: snapshot content, export order and retry. OpenRaft 0.9 spawns snapshot builds, so the newest-snapshot guard is required, not optional.
- Verify: HelperRust (`coordination`, the checkpoint export tests in `service::tests`), IrohRecoveryIntegration. Add a test with an install between clone and re-lock.
- Saving: removes a 3-5 ms store-lock hold every 3 (room host) or 8 commits from the actor's tick path, and about 500 mutex acquisitions per second per helper during games.

#### T1-5 Parked effects: check the digest once (P1)
- Ids: F-SESSION-2.
- Files: `src/session/IrohRoom.cxx:545-611`, `src/session/IrohRoomAdapters.cxx:106-118`, `src/session/IrohRoomCheckpoint.cxx:245-261`.
- Change: compute and check the digest on first sight, as today, and cache the decoded `EffectEnvelope` and the verdict on the queue entry. Later polls test the activation gate from the cached envelope. Cache the envelope, not the DOM; re-parse once at delivery, or keep a DOM only for the head. In `ActivateCommittedCheckpoint`, mark `_terminal_replay` entries when `PumpCommittedEffects` creates them, so other queued messages are not parsed.
- Keep identical: deliver, park and drop verdicts and their order. The digest stays SHA-256 over `dump()` of the DOM with `_commit` erased (`IrohRoom.cxx:584-586`). A gate-first, digest-later reorder is not allowed: a forged token with a future term or revision would park and stall the client queue instead of being dropped.
- Verify: SessionClientTransport, IrohRoomIntegration, IrohRoomQueueAcks, IrohRecoveryIntegration, SessionRecoveryGate. Add a test that a corrupt token is dropped on first sight while its checkpoint is still pending. RoomPerf.* for game-thread time.
- Saving: about 0.3-0.6 ms per parked tick for a 13 KB snapshot, times about 5-10 ticks: about 1.5-6 ms of game-thread CPU per commit per member. Up to 64 parses per activation.

#### T1-6 Table-driven base64 decode for checkpoint chunks (P2)
- Ids: F-SESSION-3.
- Files: `src/common/CoordinationBytes.hxx:46-52`, `src/session/CheckpointTransfer.hxx:52-76`, `src/session/IrohRoomCheckpoint.cxx:312-318`.
- Change: a 256-entry `constexpr` reverse table and `bool DecodeAppend(const std::string& text, std::string& out, std::size_t maximum)` writing straight into the receiver; read the field with `get_ref<const std::string&>()`; on the leader, encode from `proposalBytes_.data() + offset` without `substr`. Do not add a second worker: the decode worker is the only room worker thread (NETPLAY_INVARIANTS.md).
- Keep identical: byte-identical output and every rejection (bad character, size, non-zero trailing bits, length `% 4 == 1`).
- Verify: CheckpointTransfer, plus a new test comparing the old and new decoder on random and malformed inputs; CheckpointDecodeWorker; RoomPerf.*.
- Saving: about 0.5-1.5 ms of game-thread CPU per tick while a transfer runs, one heap allocation per chunk, about 1.4 MB of copies per received commit.

#### T1-7 Classify room messages once, at the edge (P1)
- Ids: F-SESSION-1 (canonical), F-OVH-5.
- Files: `src/session/SessionTransport.hxx:21`, `IrohRoomPeers.cxx:282-317`, `RoomMessageQueue.hxx:12-30`, `IrohRoom.cxx:18-25,545-641`, `IrohRoomAdapters.cxx:76-151`, `sf4e__SessionServer.cxx:30-44,175,276-296`, `sf4e__SessionClient.cxx:478-534`, `sf4e__SessionServer__RecoveryCheckpoint.cxx:59,66,296`.
- Change: add a tag to `session::Message` with `type`, `committed`, `verification`, and the room action `kind` and `protocolVersion` that `IsRecoveryBatchableMessage` and `SameQueuedRoomRetry` need. `ReceiveControlMessage` fills it from the parse it already does; so do the local paths that queue strings without parsing (`ServerAdapter::Send` for connection 1, `ClientAdapter::Send` when hosting, `PumpCommittedEffects`). The sniffers read the tag. `AuthorizedEffect` keeps its one parse and hands the DOM to `SessionClient` as an optional body attached only at drain, never in the 4 MB queues. Keep `AuthorizedEffect` rewriting `payload` (the 16384-byte bound at `sf4e__SessionClient.cxx:268`). Replace the hello sniff with `ClientTransport::BeginAdmission()`, called before `Send`, because today's fence opens before the hosting and peer checks. Track checkpoint sizes instead of dumping only to measure them (F-OVH-5).
- Keep identical: wire bytes, queue order and byte accounting. The digest stays over `dump()` of the DOM; reject any variant that hashes a substring of the received text.
- Verify: SessionServerTransport, SessionClientTransport, RoomMessageQueue, IrohRoomIntegration, IrohAuthorizedMatch, SessionRecoveryGate; RoomPerf.* game-thread p99.
- Saving: on each member about 3 parses and 1 dump of a 13 KB snapshot per commit, about 0.6-1.5 ms of game-thread CPU. On the leader, O(queue) parses per enqueue become O(1) (7+n today on the recovery path), about 0.1 ms per intent.

#### T1-8 Bridge rooms poll: write only the rooms that changed (P1)
- Ids: F-BRIDGE-1 (canonical), F-OVH-8, F-ROOMS-1 (bridge half), F-BRIDGE-5 (rooms-poll notification).
- Files: `server/ember/bridge/src/routes/rooms/poll.rs:92-98,131-198,229-254`, `server/ember/bridge/src/secrets.rs:110-118`, `server/ember/bridge/src/lib.rs:81-87`.
- Change: a pure `fn reported(before: &Room, report, details, invitation_unchanged, now) -> Room` that applies today's COALESCE/CASE rules in Rust; derive `PartialEq` on `Room`. Decide `invitation_unchanged` by opening the stored blob and comparing plaintext, and seal only when it differs. Issue the UPDATE only when `after != before`, reuse `after` instead of the second load, return `changed` from `apply`, and call `committed()` only then. `record_games` and the ban `INSERT OR IGNORE` still run; the close reconciliation, `PENDING_SECS` sweep and retention deletes still run every poll.
- Keep identical: `opened_at = COALESCE(opened_at, CASE WHEN opened THEN now END)` and the `WHERE closed_at IS NULL AND invitation_sealed IS NOT NULL` guard (a room still being created stays untouched). Under WAL plus `synchronous=FULL`, an unchanged poll then dirties no page and costs no fsync. Nothing depends on the 5 s `committed()` wake: SSE has its own 5 s timeout, delivery its own 1 s timeout, and the `events` Notify has no waiter. Bridge-only, no wire change, no migration, one restart, safe for `-Rollback`.
- Verify: EmberRust (bridge rooms poll tests over the fake supervisor in `server/ember/bridge/tests/common/supervisor.rs`), PublicRoomsChain. Add a test that two identical polls leave `invitation_sealed` byte-identical and emit no event. On the VPS: WAL size and fsync count over an hour with rooms open.
- Saving: about 17,280 fsyncs a day while any room is open, an estimated 10-50 KB of WAL per poll, 45 seals and 90 row loads per poll at 45 rooms, and 0.2 wake-ups per second per SSE stream.

#### T1-9 Compress the public rooms list and rankings board (P2)
- Ids: F-OVH-6 (canonical), F-BRIDGE-6, F-ROOMS-9, F-ROOMS-6, F-UI-13.
- Files: `server/ember-short/deploy/nginx/ember-short-locations.conf:70-133`, `server/ember-short/deploy/nginx/ember-short-zone.conf:12`, `server/ember/bridge/deploy/nginx/bridge.conf.in`, `rust/sf4-net/Cargo.toml:29`, `server/ember-short/deploy/site/assets/rooms.js:200-262`, `src/ui/PublicRoomsPanel.cxx:342,428`.
- Change: (1) add `gzip on; gzip_types application/json; gzip_vary on;` (optionally `gzip_proxied any`) to the `/rooms.json` and `/rankings.json` locations, and fix the stale zone comment ("kept for a minute" against a 15 s cache). (2) In-game half: enable reqwest's `gzip` feature in sf4-net and add the same gzip lines for `/v1/rooms` on the bridge vhost; keep the helper's response size bound applied to the decoded body. (3) rooms.js: skip `render()` when the body minus `listed_at` is unchanged and recompute ages from `Date.now()`; today every refresh rebuilds every card and loses keyboard focus. (4) PublicRoomsPanel: cache the sorted listing keyed by `(list.listed, region, filter)`. See section 2 for what not to do.
- Keep identical: the same bodies and cadence. Browsers handle gzip with `Vary`. The helper and the game ship together, so the reqwest feature has no compatibility window.
- Verify: on the VPS, `nginx -T` (to see whether http-level gzip already exists), `nginx -t`, and `curl -sI -H 'Accept-Encoding: gzip'` on both URLs; PublicRoomsJourney, PublicRoomsChain, HelperRust tournament rooms tests; by hand, the rooms page keeps focus across a refresh.
- Saving: about 13-17 KB per 15 s per viewer down to about 2.5-3.5 KB (estimated 75-85%), so about 1 KB/s becomes about 0.2 KB/s per tab or in-game viewer; similar for the rankings board per 30 s.

#### T1-10 Delivery workers: wake every queue, sleep until work is due (P3)
- Ids: F-BRIDGE-3.
- Files: `server/ember/bridge/src/lib.rs:80-137`, `server/ember/bridge/src/delivery.rs:104-119`, the lease functions in `delivery/results.rs`, `status.rs`, `progress.rs`, `webhooks.rs`.
- Change: give each queue its own `Notify` (or one `watch` generation all four subscribe to). Let an idle worker wait `min(next_due - now, cap)` with a 1 s floor, where `next_due` uses the exact lease predicate minus `<= now`, including results' terminal-state, `c.enabled = 1` and destination filters; a plain `MIN(delivery_next_at)` returns a past time for a queued non-terminal row and makes a hot loop. Raise the cap to 60 s only after auditing that every writer that makes work due calls `committed()` (`blumint_status`, `blumint_progress`, `matches.delivery_state = 'queued'`, the outbox, maintenance) and that no ops script writes the SQLite file directly; until then keep a 1 s cap and only fix the wake.
- Keep identical: at-least-once delivery, lease and attempt fencing, retry schedule and jitter, the 24 h budget.
- Verify: EmberRust (delivery tests, `tests/common/results.rs`, `tests/blumint_reports.rs`). Add a test that one commit wakes all four queues.
- Saving: four idle lease transactions per second (mutex holds, no fsync when idle) down to about one a minute; up to 1 s less latency on BluMint status, progress and results.

#### T1-11 Notifier inbox: index the due query and wait for the deadline (P2)
- Ids: F-PROTO-1.
- Files: `server/ember/notifier/src/lib.rs:110-117`, `server/ember/notifier/src/inbox.rs:31-36,102-147,241`.
- Change: `CREATE INDEX IF NOT EXISTS` a partial index whose `WHERE` matches the query's `discord_done = 0 OR twitch_done = 0` term (an index on `received_at` also covers the ORDER BY). When nothing is due, wait until `min(MIN(next_at), 60 s)`; keep the 60 s ceiling because the `Clock` is wall time plus an offset tests advance. Run `receive` in `spawn_blocking`, as ember-brackets does. Optional pruning: only inbox rows that are done and older than both `GIVE_UP_SECS` and the 300 s webhook tolerance; keep `matches` rows.
- Keep identical: every event recorded before the 2xx, `received_at` order, the RETRY table, `GIVE_UP_SECS`.
- Verify: EmberRust (notifier tests `notifier.rs`, `discord_room.rs`, `twitch_room.rs`); `EXPLAIN QUERY PLAN` shows the index.
- Saving: about 86,400 full scans a day that read every stored body, down to a few indexed lookups per event.

#### T1-12 Make answer types tolerant of unknown fields (P2)
- Ids: F-PROTO-6 (canonical; T2-15 and T3-3 depend on it).
- Files: `server/ember/protocol/src/partner.rs:33-52`, `rooms.rs:200-481`, `skill.rs:128-166`, `play.rs:111,347`, `event.rs:170`; `server/ember/brackets/src/bridge.rs:160-240`, `sets.rs:134-137`; `server/ember/notifier/src/bridge.rs:76-102`, `format.rs:298`.
- Change: requests keep `deny_unknown_fields`; signed objects keep it (`Binding`, `Permit`, `RoomTicket`, `GameReport`, `Challenge`, `Proof`). Answers and deliveries drop it: `LookupAnswer`, `FoundPlayer`, `FoundAccount`, `MatchResult`, `ResultParticipant`, `RoomSummary`, `RoomList`, `ConnectionRoom`, `ConnectionRoomList`, `Standing`, `Board`, `WatchAnswer`, `ClaimAnswer`, `PrepareAnswer`, and the `Event` envelope (the webhook HMAC covers the raw body). Flag `SigningKeys` for a decision rather than changing it. `ErrorCode` stays strict. Delete brackets' mirror structs except `MatchView`, fix the comment at `partner.rs:36-39`, and flip the `RoomList` test at `rooms.rs:967` to "ignored".
- Keep identical: every byte the bridge sends. This is forward-only: helpers already released still refuse unknown answer fields (section 5).
- Verify: `cargo test` in server/ember (protocol, notifier, brackets) through EmberRust; grep for tests that parse answer types with extra fields.
- Saving: about 120 lines of mirror structs, and a latent notifier outage removed: `render_room` decodes a strict `ConnectionRoom`, so any field added to room event data silently stops room announcements.

#### T1-13 Tournament requests: route identity, local and bridge jobs at submission (P2)
- Ids: F-TOUR-1, F-TOUR-11.
- Files: `rust/sf4-net/src/tournament/mod.rs:269-271,404-500` (the finding's line numbers are stale), `rust/sf4-net/src/tournament/sign_in.rs:54-82`.
- Change: `Handle` gets identity, local and bridge senders and `submit` picks by an exhaustive `Request::lane()`. Local jobs (`BridgeList`, `BridgeForget`, `MatchLeave`) run without an HTTP permit; only bridge jobs take one. Generate `op()` from one list and move `end_unopened` into `sign_in::Routes`. Do not split the enum with `#[serde(untagged)]`; it buffers the passphrase outside `Zeroizing`.
- Keep identical: identity jobs stay serial and FIFO; bridge jobs stay bounded and FIFO (tokio's `Semaphore` is fair); a full queue still answers `busy`. The native side matches answers by `request_id`, so no cross-lane order is relied on. The existing `identity_busy` hazard from `prove`'s `try_lock` is unchanged; say so in the change.
- Verify: HelperRust (`tournament` modules, `rust/sf4-net/tests/tournament_support`). Add a test where `identity_status` answers while four bridge requests hang.
- Saving: removes head-of-line waits of up to 45 s for identity and local answers during bridge trouble.

#### T1-14 Savestate: stop the nested ClearKey from churning trackedKeys (P2)
- Ids: F-SF4E-7.
- Files: `src/sf4e/sf4e__Game.cxx:36-55`, `src/sf4e/sf4e__Game__Battle__System__SaveState.cxx:790-799`, `src/sf4e/sf4e__MementoGuards.cxx:95`.
- Change: in `fKey::Initialize`, set a RAII-restored `initializingKey = this`; `fKey::ClearKey` skips `trackedKeys.erase` when `this == initializingKey` and still calls `NoteEngineClear`. Key on pointer identity, not a depth counter, which would also suppress erases of other keys. A sorted vector in address order is an optional later step.
- Keep identical: the tracked set after every engine call (`Initialize` re-inserts anyway, `Game.cxx:51`); INV-SAVE-ROUNDTRIP and INV-MAIN-THREAD. Nothing iterates `trackedKeys` during the native call. If the routing does not exist, the guard is never hit.
- Verify: MementoGuards, GgpoDelayRollback and its four variants, RollbackAudit, VfxObjectMemento, AfterimageMemento, HudMementoBounds. Count inserts per save in a debug build to size the saving; IDA was down, so the ClearKey routing rests on SAVESTATE_FREE.md.
- Saving: about 180 allocator calls per simulated frame, times (1 + rollback depth), inside GGPO callbacks.

#### T1-15 Menu tick: read only the device identity and cache misses briefly (P2)
- Ids: F-SF4E-9.
- Files: `src/sf4e/sf4e__NetplayRuntime__Input.cxx:80-160`, `src/sf4e/sf4e__OverlayPrefs.cxx:213-224,316`, `src/sf4e/sf4e__InputDevices.cxx:88`, `ControllerInventory.hxx:50,67`.
- Change: `OverlayPrefs::DeviceIdentity()` returns only the identity under the lock. Remember a failed load with a time-bounded negative cache that retries every few seconds, not "until Save", so a transient lock failure while the launcher writes cannot pin defaults. Scan the keyboard only while capture is not Idle or the Controllers page is shown, and in the same gate stop `Build` from reporting a keyboard `pressed` it no longer measures. Rebuild `pads` only when the inventory inputs change.
- Keep identical: device choice, capture outcomes, the pressed indicator on the Controllers page.
- Verify: OverlayPadCapture, ControllerNavigation, DirectInputFilter, UiPreferencesStore. By hand: remove the `overlay` object from settings.json and confirm with Process Monitor that the game no longer opens `settings.lock` every tick. In game: a fresh profile through the main menu and one fight.
- Saving: on a fresh install, a settings.json read and parse on every game-thread tick, fights included, until the first overlay save; about 15k `GetAsyncKeyState` calls a second at the main menu.

#### T1-16 Share the room snapshot instead of copying it every tick and every frame (P2)
- Ids: F-SF4E-5 (canonical), F-UI-4.
- Files: `src/sf4e/sf4e__NetplayRuntime__Status.cxx:67-75,404,546-550`, `src/sf4e/sf4e__Overlay.cxx:274-409`, `src/ui/ApplicationShell.hxx:45,293`, `src/session/sf4e__SessionClient__Room.cxx:388-397,435`, `src/ui/RoomPanel.cxx:42-46`, `src/ui/RoomControls.cxx:183-207`.
- Change: publish `std::shared_ptr<const room::Snapshot>`, rebuilt only when `(room revision, roomEpoch, classEmblems, ServerOwned())` changes; the badge strip makes it one memoized derived copy per change, and `members` is rebuilt with it. The chat carry-over on `chatUnchanged` shares the previous vector. Draw side, smaller form first: pass `const room::Snapshot&` into Draw, as Background already does, plus an elapsed-ms value for the three readers of the in-place patch (`IdleText`, `HoldStatus`, `DescribeReadyDeadline`). Do not turn all of `ShellView` into references in one step.
- Keep identical: `PresentationSnapshot` owns immutable values the drawing thread holds across a frame; `SessionClient` only replaces the room by whole assignment, never mutates a published one. Idle and hold numbers come out the same at draw.
- Verify: OverlayPresentation, OverlayLifecycle, RoomPanelNavigation, ShellJourney, PublicRoomsJourney, UiRender. In game: spectating with the menu open, where the copy runs on the render thread of a live game.
- Saving: typical room with the menu open, about 3k-7k allocations a second (12k-30k in a full room with full chat); about 2-5 MB/s of game-thread memcpy at menu rate in a busy room.

#### T1-17 UI on hidden match frames: allocation-free chat diff and probe guard (P3)
- Ids: F-UI-7 (parts 1, 2 and 4).
- Files: `src/ui/ChatTranscript.cxx:24-70`, `src/ui/MenuFeedback.hxx:31-40`, `src/ui/RoomPanel.cxx:565`, `src/ui/MatchHud.cxx:179,259`, `src/ui/GameMenu.cxx:271`, `src/ui/MenuDialogs.cxx:55,230`, `src/ui/RoomCards.cxx:42`.
- Change: (1) make `ChatTranscript::Diff` allocation-free: reuse a member-id buffer and keep `present_` as a sorted vector of at most 16; do not use the proposed revision-key early return, because `room.revision` is not proven to move on every member, host and score change. Cache the unread count. (2) In `MenuVisualFeedback`, compare label, value and flags field by field instead of building a signature string; no hash. (4) Add `MenuProbesActive()` and guard the probe-only string building and text measures. Skip part (3) except an early return while `userGlyphs` is empty.
- Keep identical: transcript lines, smoothing, and probe output in tests.
- Verify: UiRender, RoomPanelNavigation, ShellJourney, ControllerNavigation.
- Saving: about 30-40 allocations per hidden match frame (about 2k a second) on the game's render thread, and 100-150 per drawn frame.

#### T1-18 Localization lookups without a heap allocation (P3)
- Ids: F-UI-6 (canonical), F-SF4E-14.
- Files: `src/common/Localization.cxx:181,235-239`, `src/common/Localization.hxx:15`, `src/sf4e/sf4e__UserApp.cxx:427-431`, `src/sf4e/sf4e__NetplayRuntime__Status.cxx:184,538`.
- Change: look up through a `thread_local std::string` scratch key (`key.assign(id)`) with the same `find`. Resolve the `ObserveControlPlane` text only on the loss edge by passing the catalog id. Do not build a `string_view` index for all 14 catalogs (about 1-1.5 MB in a 32-bit process) and do not use a transparent hash (C++20; the target is `cxx_std_17`).
- Keep identical: returned strings, English fallback, id echo for a missing key.
- Verify: Localization, UiRenderLocales.
- Saving: one allocation per lookup, about 10k-20k a second with the menu open, plus HUD frames and one per outer tick.

#### T1-19 Stable publication: require the stable channel (P2)
- Ids: F-TOOLS-3.
- Files: `scripts/github-release.ps1:6-46`, `scripts/release-team-build.ps1`, `scripts/package-team.ps1:8-16`, `scripts/build-current.ps1:31`.
- Change: in github-release.ps1 (and release-team-build.ps1 if it is used for Stable), resolve `Get-EmberBuildTarget` and require `channel -eq 'stable'`. In package-team, assert that the receipt's `stagingBridge` matches the channel (true only for nightly). package-team keeps accepting both channels because Nightly packages through it.
- Keep identical: every legitimate Stable and Nightly run.
- Verify: test-build-provenance.ps1 and test-nightly-publication.ps1; add a case where github-release from the sf4-nightly target refuses before any upload. No in-game check.
- Saving: none; closes the path described in section 5.

#### T1-20 Bump Nightly to 1.5.0 and enforce the minor-ahead rule (P3)
- Ids: F-TOOLS-4.
- Files: `CMakeLists.txt:7`, `scripts/NightlyOperations.ps1:507-511`, `docs/development/NIGHTLY.md:7`.
- Change: set `VERSION 1.5.0` on nightly. In `Invoke-NightlyPublish`, read `origin/release:CMakeLists.txt` and refuse unless Nightly's minor is Stable's minor plus one; fail closed when `origin/release` is unavailable (the `-Local` path does not fetch it).
- Keep identical: label format and every existing check.
- Verify: test-nightly-publication.ps1 with a new equal-version refusal case.
- Saving: none; see section 5.

#### T1-21 Remove the test-only copies of the voter and retirement rules (P2)
- Ids: F-TEST-2.
- Files: `rust/sf4-net/src/service/tests.rs:4-52,2485,3007`, `rust/sf4-net/src/service/members.rs:50-101,158,439-505,595-615,737-797`.
- Change: add the case "an excluded current voter is dropped" (`admission_voters({1,2,3}, [1,2,3,4], 3, excluded 2 and 3) == {1,4}`) to the existing `mod tests` in members.rs and delete `stable_voters`. For the 3007 test, either drive the real path (`spawn_membership_operation` or `schedule_pending_membership_reconciliation`, passing the matching epoch, room, term and committed revision, or the completion is silently dropped) or call `apply_confirmed_retirements` directly with an explicit set; then delete `apply_pending_retirements`. The 3007 test already exercises the production tombstone aging; neither form proves that `members.rs:443` passes `pending_retired || retired`.
- Keep identical: production code is untouched.
- Verify: HelperRust (`service::members`, `service::tests`).
- Saving: 49 lines; two tests start checking production rules.

### Tier 2: worth doing, larger or needs a measurement or decision first

#### T2-1 Raw-body IPC frames for checkpoint chunks and room payloads (P3)
- Ids: F-SVC-14 (canonical for IPC encoding), F-OVH-4, F-SESSION-5.
- Files: `rust/sf4-net/src/wire.rs:10-96`, `ipc.rs:47`, `stdio.rs:133-136`, `service/entry.rs:39-65`, `service/checkpoints.rs:464-532`, `src/platform/HelperClient.cxx`, `HelperClientPosix.cxx`, `src/session/IrohRoomPeers.cxx:284-330`, `IrohRoomCheckpoint.cxx:309-325`.
- Change: a second frame kind carrying a compact JSON header and a raw body; fork `read_ipc`/`write_ipc` or mark the body inside the payload; bump an IPC-only version in the bootstrap, which both sides check, so a mismatched pair fails closed. Keep the credit window, `native_json_depth_ok` and `LIFECYCLE_EVENT_RESERVE`. Do T1-6 first.
- Keep identical: never bump the shared `wire::VERSION`; it heads QUIC control frames and game UDP packets too. The Linux room host compiles the same IrohRoom files and builds with its helper from one tree.
- Verify: HelperRust wire and ipc tests, HelperProcess, CheckpointTransfer, SessionClientTransport, IrohRoomIntegration, RoomPerf.*.
- Saving: about 150 KB of pipe per commit per member at 16 and no base64 on either side. No round trips are saved; the gain is game-thread CPU, mostly in large rooms after T1-1.

#### T2-2 Leader: do not echo its own commit back over the pipe (P3)
- Ids: F-COORD-6.
- Files: `rust/sf4-net/src/service/checkpoints.rs:331,585`, `recovery.rs:741`, `src/session/IrohRoomCheckpoint.cxx:124-126`, `IrohRoom.cxx:429,714-716`.
- Change: when the committed record is the pending proposal, emit a small `checkpoint_committed_local` marker in the transfer's slot of the `pending_checkpoint_committed` order; native decodes its retained `proposalBytes_`. Fall back to the full transfer whenever `proposalBytes_` was cleared (term change, checkpoint-scope error) or the identity differs; the fallback is mandatory.
- Verify: IrohRecoveryIntegration, PublicRoomHost, RoomPerf.*. The Linux room host is the public room leader and gains most.
- Saving: about 530 KB of pipe and 25+ events per commit on the leader; native decode is unchanged.

#### T2-3 Compress at propose time with a binary coordination codec (P2)
- Ids: F-COORD-1 (full change), F-COORD-2.
- Files: `rust/sf4-net/src/coordination.rs:62-162,189,423-477,1021-1053,1577`, `coordination_iroh.rs:27,659-703`, `invite.rs:493`, `src/sf4e/sf4e.cxx:46,232`.
- Change: compress the checkpoint once in the spawned propose task; binary append and snapshot envelopes without base64 or the outer JSON; keep the declared-length, `total_in` and `take(MAX_SNAPSHOT + 1)` bounds; inflate and validate UTF-8 and length in the append decode path outside the store mutex; keep `Proposal::digest` over the raw text; bump `SNAPSHOT_MAGIC` to `sf4rs002`.
- Keep identical: the same-build gate compares the SHA-256 of Sidecar.dll only, not sf4-net.exe, so a helper-only change can share a "build". Bump the ALPN to `sf4e/coordination/2` and add a helper wire version to the build identity compared at invite and admission. Public rooms rely on the procedural rule that `builds/<build_id>/` stages a room host and helper from one commit.
- Verify: HelperRust coordination tests, IrohRecoveryIntegration and its relay variant, PublicRoomsChain, plus a cross-build refusal test. Do after T1-2.
- Saving: about 25% of append and snapshot bytes plus about 10% from no double escape; about 450 KB of leader uplink per commit at 16 (estimate).

#### T2-4 Event-driven Windows helper pipe worker (P2)
- Ids: F-SESSION-6 (canonical), F-OVH-9.
- Files: `src/platform/HelperClient.cxx:40-181` (the finding's line 251 does not exist; the file has 184 lines), `HelperClient.hxx:24`, `HelperClientPosix.cxx`.
- Change: one pending overlapped header read on a persistent event, an auto-reset `sendWake_` set by `Send`, and `WaitForMultipleObjects`; do not post the read while a frame is held back (that is the backpressure); separate OVERLAPPED objects for read and write; `CancelIoEx` on Stop; keep the id-1 auth on the blocking path; `Send(std::string)` by value, which also lets the caller's temporary, possibly holding a passphrase, be wiped; one shared `HelperFrame.hxx`.
- Measure first: the timer resolution inside SSFIV.exe (only tests call `timeBeginPeriod`). It decides whether each wait is about 2 ms or about 15.6 ms.
- Verify: HelperProcess, HelperErrorScope, SessionClientTransport, IrohRoomIntegration, RoomPerf.* checkpoint transfer time. Worker thread only.
- Saving: 64-500 idle wake-ups a second to near zero, and up to about 4 ms (1 ms resolution) or about 31 ms (15.6 ms) per checkpoint credit window, which also shortens T1-5's parked ticks.

#### T2-5 Room host: sleep until the next room deadline (P2)
- Ids: F-ROOMS-2.
- Files: `server/roomhost/main.cxx:26,117-146`, `server/roomhost/RoomHost.cxx:241-248,346`, `src/session/RoomModel.cxx:840-852`.
- Change: replace `Quiet()` with `RoomHost::WaitMs(nowMs)`: 16 ms while a table is Playing or Paused, past Idle but not yet Playing, anything is in flight (`pendingBans_`, `ProposalInFlight`, `CheckpointPending`, `HasRecoveryCandidate`, `PendingProposal`, `recoveryFailedSinceMs_`), closing, or a helper send waits on a full queue; otherwise `min(250 ms, NextDueMs)` with `RoomAuthority::NextDueMs` beside `HasDueTimerTransition`.
- Measure first: per-room CPU of occupied idle rooms with `server/roomhost/soak/soak-monitor.sh`, and audit `SessionServer::Step` and match-authority phases for time-driven work outside `HasDueTimerTransition`.
- Keep identical: every helper event ends the wait at once; timers fire on time with `NextDueMs`; stdin EOF is noticed within 250 ms, inside the supervisor's 10 s allowance.
- Verify: RoomHostConfig, PublicRoomHost, PublicRoomHostOwnDelay, SessionServerReadyTimeout, RoomReadyTimeout. Ships with a room host build per `build_id`.
- Saving: about 58 of 62 wake-ups a second per occupied idle room; about 10% of a core across 45 rooms (assumption).

#### T2-6 Supervisor per-room change counter (P2)
- Ids: F-ROOMS-1 (supervisor part).
- Files: `server/ember-rooms/src/supervisor.rs:369-394,445`, `server/ember-rooms/src/api.rs:267`, `server/ember/bridge/src/routes/rooms/poll.rs`, `server/ember/bridge/src/routes/rooms/supervisor.rs:78-101`.
- Change: bump a per-room `rev`, seeded per supervisor process, in `set_live` only when a reported field differs. The bridge caches a room's `rev` only when its UPDATE matched a row and the transaction committed, drops the entry on create and close, and treats a missing `rev` as "always apply".
- Keep identical: additive both ways (`Reported` has no `deny_unknown_fields`; the out-of-tree pool passes room dicts through). Deploy VPS supervisor, server1 supervisor and bridge in any order. Do after T1-8.
- Verify: ember-rooms tests (`server/ember-rooms/tests/rooms.rs`), bridge poll tests including the create-window race.
- Saving: after T1-8 only CPU: the per-room seal, details parse and no-op inserts per poll.

#### T2-7 Room broadcast: stop copying chat per recipient, then build the shared part once (P2)
- Ids: F-SESSION-13.
- Files: `src/session/sf4e__SessionServer__Rooms.cxx:391-433`, `src/session/RoomModel.cxx:37-38`.
- Change: (1) now: `SnapshotFor(member, withChat)` skips the chat copy when it would be cleared. (2) later: encode the shared fields once and splice per recipient; the output must equal `json(snapshot).dump()` byte for byte, because the leader digests that dump and members re-dump before comparing.
- Verify: for (2), pin byte equality on the RoomHostBench 16-member fixture for `chatUnchanged` true, false and `--legacy-clients`; SessionServerTransport; RoomPerf.*.
- Saving: (1) about 16 x 25 KB of chat copies per commit on the leader; (2) an estimated 2-4 ms of the 6.3 ms host step.

#### T2-8 Discord presence: rebuild only when something changed (P2)
- Ids: F-SF4E-8 (canonical), F-UI-15.
- Files: `src/sf4e/sf4e__NetplayRuntime__Status.cxx:196-226`, `src/discord/Presence.hxx:9`, `src/discord/main.cxx:42-105`.
- Change: widen `discordPresenceKey` with a hash of `DiscordInvitation()`, the `HoldsMatchRoom` bit and `serverOwned`, which change without a key change today. When the key is unchanged, resend the cached string at the existing 1 s cadence without building `PresenceInput`, running `Describe` or building JSON. `PresenceInput` takes const references. In the companion, re-serialize only on receive or at `expires`, and keep the last `show` and `secret` instead of parsing `lastSent`. No new message type: the companion treats any other type as fatal.
- Keep identical: the companion exits after 15 s of silence and is started once per launch, never restarted (`sf4e__NetplayRuntime.cxx:363-366`); keeping the 1 s cadence leaves that margin untouched.
- Verify: DiscordPresence, DiscordBridge. In game: an invitation renewal and a tournament watch transition must still refresh or hide the secret.
- Saving: one room copy of about 35-40 KB, one `Describe` and one JSON build per second on the game thread; about 100 dumps a second in the companion.

#### T2-9 Helper departure grace: stop exporting, then group per-room state (P2)
- Ids: F-SVC-1.
- Files: `rust/sf4-net/src/service/checkpoints.rs:265-275`, `service/mod.rs:624-918`, `service/departure.rs:582-645`, `service/entry.rs:95`, `service/tests.rs:270`.
- Change: first, as its own deliberate one-line behavior change with a test, return early from `pump_committed_checkpoint` while `retirement_started.is_some()`; grace exists only to answer authority RPCs. Then a `RoomState` struct and `Phase` enum as a pure move that reproduces today's grace survivors exactly: `room`, `recovery`, `epoch`, `short`, `refresh_roster`, `seats`, `join_*`, `departure_failed`, `closed_generation`, `last_exported_revision`, `latency`.
- Verify: HelperRust (`service::tests` leave, rejoin and departure), IrohRecoveryIntegration.
- Saving: a wasted export, hash, base64 and 500 Hz tick for up to 30 s per leave; about 120 lines.

#### T2-10 Bridge: fail closed on unknown stored enum values (P2 for match state, P3 for role)
- Ids: F-BRIDGE-9. Quiet hardening; keep it out of release notes.
- Files: `server/ember/bridge/src/auth.rs:174-178`, `routes/ledger.rs:82-86`, `routes/lobbies.rs:84`, `tournament/mod.rs:73`.
- Change: `FromSql` for `Role`, `MatchState`, `DeliveryState`, `Rotation` and `Format` returning an error on unknown text; list paths skip and log a bad row. The match-state part is the reachable one (section 5).
- Verify: EmberRust (ledger and auth tests). Add a test that inserts an unknown state and expects a 503 on that match and a list that skips it.
- Saving: none; five silent fallbacks removed.

#### T2-11 Bridge read-only connection pool (P3)
- Ids: F-BRIDGE-4. Do after T1-8 and T1-10, and only if request latency is measured to suffer.
- Files: `server/ember/bridge/src/db.rs:61-127`, `routes/blumint/liveness.rs:124`.
- Change: 2-4 `SQLITE_OPEN_READ_ONLY` connections for `Db::read`, each with its own `busy_timeout`; the writer stays behind its mutex. `liveness.rs` opens `:memory:`, which cannot be shared; use a temp file or a shared-cache URI.
- Verify: all EmberRust bridge tests; request latency on the VPS. Saving: request latency no longer includes queued writes and fsyncs.

#### T2-12 Bridge: notify only after writes that queued work (P3)
- Ids: F-BRIDGE-5 (the rooms-poll part is in T1-8).
- Files: `server/ember/bridge/src/lib.rs:76-87`, the 34 `committed()` call sites, `routes/mod.rs:225-252`.
- Change: a transaction flag set by `events::emit` and the queue writers; `AppState::write` notifies only when it is set; delete the 34 manual calls and the dead `events: Notify`. Needs the same writer audit as T1-10. State explicitly that session revocation does not notify today, so the 5 s stream close bound is unchanged.
- Verify: EmberRust (events and delivery tests). Saving: idle SSE streams wake only on their 5 s timeout.

#### T2-13 Authority claim coalesce window (P3, owner's decision)
- Ids: F-OVH-7.
- Files: `rust/sf4-net/src/coordination.rs:63-65,1233-1273`, `rust/sf4-net/src/recovery.rs:30-33,407-452`.
- Change: (a) set `AUTHORITY_CLAIM_COALESCE` to about 1.2 s. This is a timing change: the leader may report `writable` from a claim up to 1.2 s old instead of 0.5 s, still under `AUTHORITY_READ_TIMEOUT` = 2 s; Raft commit stays quorum-gated, so only how fast a room pauses moves. (b) Defer the learner shortcut; it changes quorum-loss inputs and only helps while commits flow.
- Verify: IrohRecoveryIntegration, HelperRust coordination tests. Saving: about 2-12 KB/s; do last.

#### T2-14 Claim renewal: stable binding, then fewer round trips (P3)
- Ids: F-TOUR-5 (canonical), F-PROTO-13.
- Files: `server/ember/bridge/src/routes/play/room.rs:195-224`, `rust/sf4-net/src/tournament/client.rs:386-453`, `tournament/play.rs:160-194,303-333`, `server/ember/protocol/src/play.rs:112,184`.
- Change, in order: (1) the bridge keeps the binding's `issued_at` stable per binding revision (nothing reads a binding's `issued_at`; the helper uses the permit's). (2) The helper skips `verify` only when the incoming signed binding is byte-identical to the held one. (3) Optional: `bound_revision` in the claim and `binding_current` in `ClaimAnswer::Room` with `skip_serializing_if`, sent only when the claim carried `bound_revision`, and the "current" test also compares `room_id`. (4) Optional: a feature-gated session-only renewal after a security review; the first claim stays proven and `endpoint_id`, `helper_instance_id` and build cannot change.
- Verify: EmberRust play tests, HelperRust tournament tests. Saving: about 1 KB and one round trip per renewal, about 0.1 KB/s per fighter.

#### T2-15 Typed event data for the notifier (P2)
- Ids: F-PROTO-5. Do after T1-12.
- Files: `server/ember/protocol/src/event.rs:182-185`, `server/ember/notifier/src/format.rs`, bridge event writers (`tournament/progress.rs:165,263`, `routes/lobbies.rs:445`).
- Change: typed data structs per announced kind, tolerant of unknown keys; emit `null` wherever the `json!` literal emits `null` today (no `skip_serializing_if` on such fields); the flattened room event needs a tolerant `ConnectionRoom`. Events are stored JCS-canonical, so field order does not reach the wire.
- Verify: notifier tests plus one fixture test per kind's current JSON. Saving: about 150 lines; regressions become test failures.

#### T2-16 Brackets worker: wait for the earliest deadline (P3)
- Ids: F-PROTO-2.
- Files: `server/ember/brackets/src/bracket.rs:38,185-190`, `schedule.rs:39`, `reconcile.rs`, `README.md:28,269`.
- Change: wait `min(earliest deadline, 60 s)`; keep `WAKE_SECS` as the ceiling because the clock is advanceable. Say in the README that a `poll_secs` below 60 now spends more of the 500-a-month Challonge budget.
- Verify: brackets tests in EmberRust. Saving: 1,440 idle wakes per tournament per day; reads land on time.

#### T2-17 One GGPO frame step core for live, resim, orphan and stress (P2)
- Ids: F-SF4E-1. Determinism surface: review separately from the overhead work.
- Files: `src/sf4e/sf4e__Game__Battle__System.cxx:338-417`, `sf4e__Game__Battle__System__Ggpo.cxx:278-301,621-697`, `RollbackStress.cxx:176-183`.
- Change: a shared core with a `syncSoundBeforeStep` flag (the orphan path runs the pre-step `SyncState`, the stress harness does not); `ReadGgpoInputs` only synchronizes and records diagnostics; classification, `DrainPendingAbort`, `EndSpectatorStream` and abort-but-advance stay in each caller, and `NoteDisconnectFlags` runs after the caller's policy; narrow the resim playback guard to the engine update, as the live path does; fold in `StressSimulate`.
- Keep identical: INV-UNDETOURED, INV-INPUT-SYMMETRY, INV-PURE-SOUND, INV-NO-CLOSE-IN-CALLBACK, INV-CALLBACK-INERT, INV-ORPHAN; `EndSpectatorStream` never reachable from the callback.
- Verify: GgpoDelayRollback and variants, RollbackAudit, RollbackDiagnostics, GgpoGate, ReplayInputs; then a two-PC match. Unverified: whether any engine memento record reads pads. Saving: about 15-20 lines and one desync hazard.

#### T2-18 Session role and spectator drain as named state (P2)
- Ids: F-SF4E-3.
- Files: `src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:103,464,638,702,735,823,837`, `sf4e__NetplayFacade.cxx:238`, `MementoGuards.cxx:45`, `System.cxx:663-666`.
- Change: draining becomes a flag on top of `PHASE_BATTLE_CLOSING` (both `ggpo_free_buffer` and memento `Fail` test that phase during the drain), cleared by `OnSessionClosed`, `CancelDeferredGgpoClose` and `ShutdownNetplay`. A `SessionRole` set where `localPlayerHandle` is assigned, reset in `RetireGgpoSession`. Do not fold `LocalIsSpectator()` or `netplay->spectating` into it; those are seat concepts.
- Verify: GgpoGate, GgpoSpectatorCapacity, GgpoSpectatorHandleControl, GgpoSpectatorBacklog, RecoveryController. Keep INV-CALLBACK-INERT exactly.
- Saving: about 25 scattered predicates become two named concepts, and the GGPO callbacks stop depending on the facade.

#### T2-19 Runtime gates computed in one place (P2)
- Ids: F-SF4E-2.
- Files: `src/sf4e/sf4e__NetplayRuntime__Room.cxx:110-296,448`, `sf4e__NetplayRuntime__Status.cxx:404,648`, `sf4e__NetplayRuntime.cxx:661,771-795`, `sf4e__Pad.cxx:110`.
- Change: a pure `ComputeGates`. Computing it where `Publish` runs leaves `Dispatch` one tick stale as today; for current gates, compute before `DrainCommands` and after each state-changing command, and list every outcome change, the Ready and Rematch retry branch included. Review `spectator_runtime_test.cxx:540,611`.
- Verify: SessionController, OverlayPresentation, spectator runtime tests. No determinism effect; consistency only.
- Saving: about 25 atomic loads per tick (negligible); one place that decides what the player may do now.

#### T2-20 Runtime dispatch over a typed command (P2)
- Ids: F-SF4E-4.
- Files: `src/sf4e/sf4e__NetplayRuntime__Room.cxx:82-500`, `sf4e__Overlay.cxx:267-273,424-450`, `sf4e__NetplayFacade.hxx:101-146`.
- Change: a `std::variant` of command families; `RoomCommand` carries `discordRevision`, `preferences`, `publicTicket`, `createdRules`, `join`, `practiceRoom`, `delayPolicy`, `character`, `stage`, `randomPool`, `roomAction` and `selectedDelay` as payload; the hidden-shell submit keeps identity winning over a room command; parked intents hold the variant; the generation check stays in exactly the handlers that pass it today.
- Keep identical: each family's handler body moves unchanged; the mailbox byte cost stays supplied by the caller.
- Verify: SessionController, OverlayLifecycle, ShellJourney. Pairs with F-UI-5 in T3-15.
- Saving: sf4e__NetplayRuntime__Room.cxx from about 880 to about 450 lines; sentinel ordering disappears as a concept.

#### T2-21 One session start sequence (P2)
- Ids: F-SF4E-11.
- Files: `src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:376-555`, `src/sf4e/sf4e__UserApp.cxx:249-269`.
- Change: `BeginSession(label, spectator)` with the invalid-roster early return first, then the leftover retire, then `matchTelemetry.Reset(spectator)`; the two `Start*` return bool and the caller aborts once with the most specific reason. Decide deliberately whether failure sets `nNextBattleStartFlowTarget` and `bTerminateOnNextLeftBattle` (spectator does today, fighter does not). Only `StartGGPO` applies `GgpoDisconnectTolerance`. The failure notice becomes the specific one, as intended.
- Verify: GgpoSpectatorCapacity, GgpoSpectatorHandleControl, GgpoOutageTolerance, GgpoSyncRetry.
- Saving: about 35 lines and one session-start order.

#### T2-22 Screen panel interface for owner-routed phases (P2)
- Ids: F-UI-1 (canonical), F-UI-10 (the RoomEntries split).
- Files: `src/ui/ApplicationShell.cxx:256-946`, `IdentityPanel.hxx:103`, `PublicRoomsPanel.hxx:57`, `RankingsPanel.hxx:43`, `ReplaysPanel.hxx:39`, `RoomPanel.cxx:164-451`.
- Change: route Rows, Status, Activate, Adjust, Choose, Shortcut, Card and Detail through the owning panel. `Update` and `IdentityPanel::Hidden` stay fan-out calls to every panel in today's order. Keep the shell's precedences: room screens first for Shortcut, Chosen and Adjust; `retry-save` excluded from Identity; PublicRooms Status also on "create"; an Activate that returns false falls through to the shell's id chain. `TakeFocus` and the `pr-opening` move keep their place. Leave the SettingsScreens move for later.
- Verify: hash the draw lists across ControllerNavigation, RoomPanelNavigation and NativeSelectionAvailability before and after. No overhead saving.
- Saving: about 120 lines and 26 dispatch branches leave ApplicationShell.cxx.

#### T2-23 Stable publishes through draft, verify, publish (P2)
- Ids: F-TOOLS-5. Do after T1-19.
- Files: `scripts/github-release.ps1:16-51`, `scripts/NightlyOperations.ps1:250-290,507`, `scripts/BuildEnvironment.ps1`.
- Change: generalize the Nightly release state machine; bind the repository to the channel from `Get-EmberBuildTarget`, never to a parameter; keep retention Nightly-only; set `Latest` only at publish; keep Stable's previous-asset digest check. This also gives a resumable path for the known gh HTTP 500 on the 393 MB upload.
- Keep identical: Stable's assets, title, notes, `--latest`/`--prerelease` flags and refusal of existing releases; Stable becomes a draft for about a minute before it is public.
- Verify: test-nightly-publication.ps1 extended with Stable cases.
- Saving: one release publisher instead of two, about 20 lines of duplicated preconditions, and read-back verification for Stable.

### Tier 3: cleanups and decompositions

Each block lists the change per id with the adversary's condition. All are P3 unless marked. None changes runtime overhead in a way worth measuring.

#### T3-1 Coordination and transport tidy-up
- Files: `rust/sf4-net/src/coordination.rs` (forwards :1285-1404, dispatch :1473, apply :820-877), `rust/sf4-net/src/recovery.rs` (:79-111, :454, :730-1019), `rust/sf4-net/src/coordination_iroh.rs:174,632,745`, `rust/sf4-net/src/coordination/server_owned.rs:49`, `rust/sf4-net/src/transport.rs:237-328`, `rust/sf4-net/src/transport/with_proof.rs:24`, `rust/sf4-net/src/invite.rs:145-307`.
- F-COORD-7: `Coordinator::store()` and delete the forwards; `probe_reserved(&ProbeReservation)`; constructor families take `Ownership`. F-COORD-8: split coordination.rs and recovery.rs on the named seams, after T2-3 so the codec moves once. F-COORD-9: `#[repr(u8)] enum Method` with the same byte codes 1-7. F-COORD-10: unify only the predicates that are identical today; adding `pair_revision != 0` or a dedup rule to apply or install changes which entries are accepted. F-COORD-13: one guarded helper; keep the error display strings byte-identical, they reach native. F-COORD-14: first add golden accept and reject tests for `sf4e1`, `sf4e2`, `sf4e3`, `emd1`, `emd2`; keep `encode_discord`'s `bytes[..91]` truncation.
- Verify: HelperRust (`coordination`, `recovery`, `transport`, `invite`).
- Saving: about 150 lines and 19 forwarding methods (F-COORD-7), four string tables and an impossible error branch (F-COORD-9), about 40, 80 and 70 lines (F-COORD-10, -13, -14); no file over about 600 lines (F-COORD-8).

#### T3-2 Helper actor tidy-up
- Files: `rust/sf4-net/src/service/mod.rs` (:183-622, :947-1369, :1497-1536), `controls.rs:26-372`, `members.rs:334`, `refresh.rs:236-418`, `protocol.rs:236,567`, `probes.rs:53,275`, `network.rs:126`, `games.rs:166,336`, `departure.rs:503`, `entry.rs:135`.
- F-SVC-2: move types to their modules. F-SVC-3: one async dispatcher; the two tests at `tests.rs:3963-4000` need the `commands` and `failed_ipc` channels. F-SVC-5: step 1 only, delete the dead `last_control_rebound`; step 2 is unsafe because native also clears rebound on `control_closed`, in `Fail()` and on a leader mismatch. F-SVC-6 and F-OVH-10: serialize the membership payload once per publication. I checked `refresh.rs:236-238`: the leader republishes on every 1 s refresh while a retirement is pending, so F-OVH-10's premise was too narrow, but the cost stays small; steps 2 and 3 need a forced republish. F-SVC-8: phase 1 only (borrowed `RawValue`, static `kind`). F-SVC-9: derive `PartialEq` on `Admission`. F-SVC-10: one `send_coordination` helper and a named `CoordinationMarker`. F-SVC-11: typed `EndpointId`, keeping `""` for an absent leader. F-SVC-13: keep probe failure codes 1-11 and event JSON identical.
- Verify: HelperRust (`service::tests`).
- Saving: mod.rs down to about 550-650 lines; about 35, 60 and 70 lines (F-SVC-3, -10, -13); 14 of 15 membership serializations per publication; about 100 string allocations a second per helper.

#### T3-3 Tournament bridge call plumbing
- Files: `rust/sf4-net/src/tournament/client.rs` (:318-664), `play.rs` (:242-298, :563-597), `sign_in.rs:30-151`, `ranking.rs:296-330`, `rooms.rs:58-215`, `watch.rs:257-328`, `mod.rs:343-362`, `sites.rs:95`, `bridges.rs:171-195`.
- F-TOUR-2: one `BridgeCall` path; the borrowed closure needs an async-closure shape or a boxed future; keep the `rooms_unavailable`/`ranking_unavailable` 404 rule. F-TOUR-3, parts (a), (b) and (d): typed reads only for already-typed callers; pass-through answers stay untyped, by the T1-12 rule; a `RawValue` in `Event::Tournament.data` needs a custom Serialize/Deserialize, as `NativeControlMessage` has, because `Event` is internally tagged. F-TOUR-4: `Failure { Local(Code), Bridge(String) }`; `settled` must match both forms, and an unknown bridge code still yields `http_<status>`. F-TOUR-13: with F-TOUR-2.
- Verify: HelperRust tournament tests and `tests/tournament_support`.
- Saving: about 150-200 lines and about 10 per-attempt clones (F-TOUR-2); 2 of 5 JSON trees per typed answer and about 30 lines (F-TOUR-3); about 25 repeated lock closures (F-TOUR-4); microseconds (F-TOUR-13).

#### T3-4 Helper frame writes, stdio, identity status
- Files: `rust/sf4-net/src/wire.rs:40-96`, `rust/sf4-net/src/service/entry.rs:31-79`, `rust/sf4-net/src/stdio.rs:99-152`, `rust/sf4-net/src/tournament/mod.rs:515-545`, `rust/sf4-net/src/identity/mod.rs:204-224`.
- F-TOUR-6: build each frame in one buffer and write once; byte-identical frames. F-TOUR-7: an `OnEof` input policy; only `UnexpectedEof` becomes Shutdown with id `last_id + 1`, then park; a malformed frame or a non-increasing id still exits 1. F-TOUR-9: the serial task returns the identity status with its outcome; record "last sent" only when sent; never block `respond` on the std Mutex.
- Verify: HelperRust (`wire`, `stdio`, `tournament`), PublicRoomHost.
- Saving: 3 of 4 write calls per frame; one 164 KB copy and validation per room host command and about 35 lines; about 25-150 B/s of pipe during a set.

#### T3-5 Helper spool and file writers
- Files: `rust/sf4-net/src/tournament/play.rs:576-620`, `rust/sf4-net/src/tournament/spool.rs:68-115`, `rust/sf4-net/src/identity/mod.rs:569-588,793-812`, `rust/sf4-net/src/tournament/bridges.rs:416-477`, `rust/sf4-net/src/public_room.rs:75-430`.
- F-TOUR-8: stop a pass for a bridge after its first transport failure; counting files by prefix would also count unreadable files, so count only files that open or accept that. F-TOUR-10: one `durable::replace` with a no-replace mode for the spool and temp names that do not start with the spool prefix. F-TOUR-12: remove `PublicHost`, one `connect_public` with a target enum, one `unix_now` that keeps each caller's error type.
- Verify: HelperRust (`tournament::spool`, `identity`, `public_room`).
- Saving: N-1 failing requests per resend pass during an outage; about 35 and 40 lines (F-TOUR-10, -12).

#### T3-6 Protocol crate tidy-up
- Files: `server/ember/protocol/src/tournament.rs`, `play.rs` (:60-498), `report.rs:87-165`, `matches.rs:129-230`, `lobby.rs:26,77`, `rooms.rs` (:65-400), `event.rs:63,155`, `challenge.rs:61`, `skill.rs:9`; `server/ember/brackets/src/config.rs:362-426`, `server/ember/brackets/src/bridge.rs:46`; `server/ember/bridge/src/tournament/state.rs:124`.
- F-PROTO-3: move the bracket engine into the bridge; computed labels only as part of the move. F-PROTO-4: an `AttemptKey` view; `version` and observation fields stay out of the comparison. F-PROTO-8, dedup only: `is_hex` in `GameReport::check`, `sites::is_site_id`, `event::parse_rfc3339`, a shared provider-command header check that keeps the length-only `required_build_id` rule; leave `table_id <= 3`. F-PROTO-9: a `BridgeSigned` trait with kid, then check, then signature. F-PROTO-10: `bool` with default for `ready`/`watch` (an explicit `null` would now be refused; no known sender does it); the `PublishRoom` conversion must accept explicit nulls from older helpers. F-PROTO-11: one `wire_enum` macro; `ErrorCode` stays strict; `MatchState`/`DeliveryState::parse` become table lookups. F-PROTO-12: split rooms.rs with `pub use`.
- Verify: `cargo test -p ember-protocol` and EmberRust.
- Saving: about 950 lines leave the protocol crate the helper compiles (F-PROTO-3); about 40, 40 and 150 lines (F-PROTO-4, -9, -11); one rule per field (F-PROTO-8).

#### T3-7 Services' shared client and frame parsing
- Files: `server/ember/brackets/src/bridge.rs:519`, `http.rs:72-83`, `lib.rs:142`, `challonge.rs:130,378`, `service.rs:372`; `server/ember/notifier/src/bridge.rs:122`, `sent.rs:57-67`, `lib.rs:50`, `eventsub.rs:206-212`.
- F-PROTO-14: one bridge client module behind a feature that is off by default, so sf4-net does not pull it in; the shared `Clock` keeps brackets' frozen mode. F-PROTO-15: typed EventSub frames with the cheap checks before `tokio::spawn`; keep the dedupe first and leave the keepalive at Twitch's default.
- Verify: notifier and brackets tests in EmberRust.
- Saving: about 120 lines (F-PROTO-14); one parse, one subtree clone and one task spawn per chat line on a busy channel (F-PROTO-15).

#### T3-8 Bridge small overhead
- Files: `server/ember/bridge/src/routes/rooms/supervisor.rs:131`, `delivery/results.rs:60-75`, `routes/blumint/mod.rs:192`, `routes/sign_in/provider.rs:286`, `routes/rooms/mod.rs:423-468`, `routes/ledger.rs:131-166`, `routes/records.rs:66-80,252`, `routes/matches.rs:461-481`, `delivery/status.rs:35-40`, `routes/policy.rs:15`.
- F-BRIDGE-2: one shared reqwest client in `AppState`; keep the per-attempt builder only in `webhooks::post_signed`, which pins `resolve()`. F-BRIDGE-7: drop the `to_value` hop only; no listing cache. F-BRIDGE-13: a `match_wins` view (rollback-safe); `scores` still returns accepted ids for wins only. F-BRIDGE-14: load the policy with the match; `observe_loaded` receives the post-bump match.
- Verify: EmberRust bridge tests.
- Saving: one client build and TCP connect per 5 s poll; one Value tree per listing; `/v1/assignments` from 1 + 2N queries to 1; two queries per non-BluMint ledger bump.

#### T3-9 Bridge structure
- Files: `server/ember/bridge/src/routes/play/room.rs:26-43`, `routes/matches.rs` (all 977 lines), `routes/lobbies.rs` (:232-848), `routes/idempotency.rs:14-41`, `routes/records.rs` (redact), `routes/sessions.rs:419`, `events.rs:105`, `routes/webhooks.rs:178`.
- F-BRIDGE-8: promote `Json<T>`, but read `rules` and `metadata` in responses as `Json<serde_json::Value>` or a raw passthrough; typed only where the bridge reads fields; `json_extract` in `assignments`; list endpoints skip and log a bad row. F-BRIDGE-10: split matches.rs and delete `respond`/`into_response`. F-BRIDGE-11: unify the reason rule on the looser one that allows `\n`; event order stays primary then set-started. F-BRIDGE-12: a `ServiceCall` runner; idempotency stores the answer bytes, and a failed encode fails the write. F-BRIDGE-16: typed response structs, keeping the recursive redaction as a test assertion and no `deny_unknown_fields` on new shared response structs.
- Verify: EmberRust bridge tests, BridgeSdk.
- Saving: about 20 ad-hoc JSON column sites become one type; no route file over about 350-400 lines; about 40 and 120-150 lines (F-BRIDGE-11, -12); one serialization per write and one parse per replay.

#### T3-10 Bridge sign-in tables
- Files: `server/ember/bridge/src/linking/site.rs:70-122`, `server/ember/bridge/src/linking/sign_ins.rs:17`, the `skill/` queries that join `discord_*`, and a new migration.
- F-BRIDGE-15: merging `discord_*` into `site_*` with views breaks `-Rollback` (section 5). Either document "no rollback past this migration", with restore.sh as the only way back, or give the views INSTEAD OF triggers; add a foreign-key and cascade test.
- Verify: EmberRust (`tests/discord.rs`, `tests/common/sites.rs`).
- Saving: one string-spliced SQL dispatch layer; about 35 statements become static SQL.

#### T3-11 Room host and supervisor tidy-up
- Files: `server/roomhost/RoomHost.cxx:213-301`, `server/roomhost/RoomHostStatus.cxx:11,242-272`, `server/roomhost/RoomHostStatus.hxx:13-17,163`, `server/ember-rooms/src/protocol.rs:60-292`, `server/ember-rooms/src/supervisor.rs:37-108,489-651`, `server/ember-reports/src/bugsink.rs:8,116`, `server/ember-reports/src/lib.rs:227`, `server/ember-short/src/lib.rs:364`, `server/ember-short/src/main.rs:57`.
- F-ROOMS-3: split `RoomHost::Report` into one job per step; stdout lines and their order stay identical, checked by a test diffing the lines over a ban and close scenario. F-ROOMS-4: pass `details` and `games` through as `RawValue`; keep the duplicate-member test. F-ROOMS-5: a `RoomTask` struct; keep `Shutdown.started` distinct from `stdin.is_none()`. F-ROOMS-8: correct the C++ comment to 128 KiB and about 43 KB and share the limits. F-ROOMS-10: tidiness only; keep the sweep-before-refusing at the record limit.
- Verify: ember-rooms tests, RoomHostConfig, PublicRoomHost.
- Saving: a few hundred small allocations per tick during in-flight commits (F-ROOMS-3); about 30, 40 and 15 lines (F-ROOMS-4, -5, -10); one 4 MiB copy per crash report.

#### T3-12 Site scripts
- Files: `server/ember-short/deploy/site/assets/rooms.js:22-31,85`, `open.js:39,137`, `rankings.js:619`, `server/ember/bridge/src/routes/rooms/mod.rs:473-509`.
- F-ROOMS-7: one shared `site.js` for `el`, `copy` and the visible-tab poll. Put build labels only on the web-only `PublicRoomList` (for example a `versions` map), never on `RoomSummary` (section 5).
- Verify: by hand on the three pages; old cached scripts keep working.
- Saving: about 60 duplicated lines; the per-release label edit moves to configuration rather than disappearing.

#### T3-13 Game session structure
- Files: `src/common/CoordinationBytes.hxx:15-33`, `src/session/RoomDigest.cxx:13-29`, `src/session/IrohRoom.cxx` and `IrohRoom.hxx`, `src/session/IrohRoomAdapters.cxx:23-62,116`, `src/sf4e/sf4e__NetplayRuntime.cxx:396,472`, `src/session/sf4e__SessionClient.cxx:56-708`, `src/session/RoomModel.cxx`, `RoomModel.hxx`, `RoomModelJson.cxx`, `src/session/IrohMatchSession.cxx:464-514`, `src/tests/iroh_test_room.hxx:36`.
- F-SESSION-4: one cached `Sha256Hex`, keeping the portable path for the Linux room host; list both CNG-failure behavior changes in the commit. F-SESSION-7: measure with RoomPerf.* first; a single tick-start pump caps transfers at one credit window per tick; `CloseRoom`'s poll loop and the integration tests need an explicit pump. F-SESSION-8: `EpochState`, keeping `abandonPending_`, `abandonEpoch_`, `nextDecodeTicket_` and `decoder_` process-scoped; `latency_.Stop()` stays a call. F-SESSION-9: the `EffectVerdict` enum and the dead branch now; a reformat only after open branches land (the repo has no `.clang-format`). F-SESSION-10, parts (a)-(c). F-SESSION-11: a clock type with "advance capped" and "frozen" operations; permits excluded. F-SESSION-12: split RoomModel files. F-SESSION-15: a virtual `FindGame`, with the fake in `src/tests/iroh_test_room.hxx` updated; do not use `Games()`.
- Verify: SessionServerTransport, SessionClientTransport, RoomAuthority and the Room* unit tests, IrohMatchSession, IrohRoomIntegration.
- Saving: about 25, 20, 60 and 90 lines (F-SESSION-4, -7, -8, -10); SessionClient.cxx from about 836 to about 450 lines; no RoomModel file over about 650 lines; one small copy per link per tick (F-SESSION-15).

#### T3-14 Game hooks structure
- Files: `src/sf4e/sf4e__Game__Battle.cxx:200-515`, `sf4e__Game__Battle.hxx:88-93`, `sf4e__Game__Battle__System__SaveState.cxx` (:308-434, :813-872), `sf4e__NetplayRuntime.cxx:30-136`, `sf4e__NetplayRuntime.hxx:112`, `sf4e__Game__Battle__System__Ggpo.cxx`, `sf4e__Game__Battle__System.cxx`, `sf4e__Overlay.cxx:470-681`, `src/Dimps/*.cxx`.
- F-SF4E-6: per-manager sound records; the save record mirrors today's stub-only record, an unknown adapter in `IsStillPlaying` returns false, and the developer overlay readers move too. F-SF4E-10: grouped `= {}` resets kept ahead of `ShutdownNetplay`; `ResetMatchEntry` still clears `VsBattle::nextRequest`. F-SF4E-12: pure file moves; a trigger table only if it carries a per-entry key type, reset rule and "advance without opening" flag. F-SF4E-13: the `BattleFlowGlobals` struct only. F-SF4E-15: inline Dimps accessors; keep out of line any whose address is taken.
- Verify: GgpoDelayRollback and variants, MementoGuards, OverlayLifecycle, OverlayReset, SessionController.
- Saving: about 600-700 map lookups per frame become indexing, roughly 30 microseconds a frame (F-SF4E-6); about 80, 40, 35 and 400 lines (F-SF4E-6, -10, -13, -15); no hook file over about 500 lines (F-SF4E-12).

#### T3-15 UI tidy-up
- Files: `src/ui/ApplicationShell.cxx` and `.hxx`, `src/ui/RoomPanel.cxx`, `src/ui/FighterSelector.cxx:361-470`, `src/ui/TrainingPanel.cxx:52-80,285,593`, `src/ui/ChatPanel.cxx:139-196`, `src/ui/TrainingMeter.cxx`, `src/ui/SelectionArt.cxx:224-426`, `src/ui/GameMenu.cxx:15-24`, `src/sf4e/sf4e__Overlay.cxx:424-449`, `src/launcher/launcher.cxx:605-706`.
- F-UI-2: one table for the 13 on/off rows; `hud-spacing`, `visibility` and `delay-policy` stay hand-written. F-UI-3: a typed key on `MenuEntry` (code quality, about 16 small allocations per board frame at most). F-UI-5: move `RuntimeCommand`'s UI-visible part to a UI-free header only. F-UI-8: a chat log clipper with cached heights; the saving is formatting and measuring off-screen lines. F-UI-10: member objects for the shell's state clusters; today `HeldWindow`'s static scroll is shared by both callers, and AngledBar and ClassicMeter are different loops, so both need care. F-UI-11: reuse the run vector only, no inline labels; key width caches by locale, font and size. F-UI-12: a POD image key in an ordered map, or a defined eviction tie-break. F-UI-14: a link variant that still never logs room codes, wipes the URI, logs ignored replay links and nulls absent variables. F-UI-16: explicit training Load that keeps "read once, after the directory is set".
- Verify: UiRender draw-list hash, ControllerNavigation, RoomPanelNavigation, NativeSelectionAvailability, ShellJourney, SelectionArt.
- Saving: about 35, 25, 30, 40 and 50 lines across F-UI-2, -5, -14, -10; per drawn frame up to about 16 small allocations (row ids), 45-60 (image keys), 20-40 on HUD frames, and the formatting of off-screen chat lines.

#### T3-16 One loopback bridge test harness (P2)
- Files: `server/ember/bridge/Cargo.toml`, `server/ember/bridge/tests/common/mod.rs:71` and `common/supervisor.rs`, `rust/sf4-net/tests/public_rooms_support/mod.rs:54-288`, `rust/sf4-net/tests/tournament_support/mod.rs:68-157`, `server/ember/brackets/tests/support/bridge.rs:46`, `server/ember/notifier/tests/notifier.rs:325`, `server/ember-rooms/tests/rooms.rs:120-360`.
- F-TEST-3: `ember_bridge::testing` behind a `test-support` feature with a self-referencing dev-dependency; resolver 3 keeps the shipped bridge build unaffected, at the cost of one extra compile in `rust-ember-target`. F-TEST-4 (P2): one `until` helper; convert only positive polls. F-TEST-6: replace the fake supervisor's `Fake`/`handle` only; the `older_bridge_in_front` relay keeps the raw parser.
- Verify: HelperRust, EmberRust.
- Saving: about 350 lines of harness copies, 60 lines of AppState rebuilds, 150 lines of wait helpers, and under 200 lines of the second fake supervisor; a new Config field touches one place.

#### T3-17 Split the large test files (P2 for F-TEST-1 and F-TEST-8)
- Files: `rust/sf4-net/src/service/tests.rs` and `service/tests/`, `rust/sf4-net/src/service/entry.rs:95-156`, `src/tests/session_server_transport_test.cxx`, `src/tests/room_panel_navigation_test.cxx`, `src/tests/ui_render_test.cxx:354-1125`, `src/tests/iroh_recovery_integration_test.cxx:77-1068`, `CMakeLists.txt` registrations.
- F-TEST-1: about 650 lines of shared fixtures go to `service/tests/support.rs` (`led_host`, `BoundHost`, `complete_next`, `send_admission`, `GameSide`, `ReservationFixture`, `joined_invite`, `leads`, `run_probe`), single-family fixtures move with their tests; the relay dedup covers 20 sites. F-TEST-7: `Actor::new`. F-TEST-14: `phase` wrappers. F-TEST-8: five session_server test files; `TestRetiringSpectatorProjection` goes with the barriers. F-TEST-9: only blocks that do not depend on an earlier block's state become functions.
- Verify: the same test counts pass before and after.
- Saving: tests.rs from 7384 lines to a support module plus about 12 files; about 40, 65 and 200 lines (relay lookups, the Actor literal, repeated setup); a failing C++ scenario no longer hides the ones after it.

#### T3-18 Test vocabulary and labels
- Files: `server/ember-rooms/tests/rooms.rs`, `server/ember/notifier/tests/twitch_room.rs`, `src/tests/test_support.hxx` and the 29 test files that define their own `CHECK`, `server/ember/bridge/tests/blumint_reports.rs`, `startgg.rs`, `partner.rs`, `tests/common/`, `cmake/sf4e_tests.cmake`, `cmake/sf4e_helper.cmake:72-99`.
- F-TEST-5: do not convert `rooms.rs:297,319,337,369` (negative windows or a deliberate "do not look early"); bound the twitch window by the keepalive run; leave the 16 s C++ window. F-TEST-10: `EXPECT` and `CHECK_MSG` in test_support.hxx, keeping each file's stop-or-continue behavior. F-TEST-11: split blumint_reports.rs; two cross-binary `#[path]` includes move to `common/`. F-TEST-12: SQL seed helpers. F-TEST-13: CTest `LABELS`; a targeted-retest script must not write into `rust-test-target` while a gated ctest runs.
- Verify: full ctest run once.
- Saving: about 40 macro and helper definitions and about 80 lines of seed SQL; label-based targeted retests.

#### T3-19 Packaging scripts
- Files: `scripts/package-team.ps1`, `package-upgrade.ps1`, `package-installer.ps1`, `github-release.ps1`, `release-team-build.ps1`, `publish-nightly.ps1`, `tester-preflight.ps1:57-67`, `NightlyOperations.ps1`, `build-current.ps1:9-13,75`, `BuildProvenance.ps1:63`, `test-build-provenance.ps1:86`, `test-nightly-publication.ps1`; `CMakeLists.txt:902-911`.
- F-TOOLS-1: packaging functions returning objects; pin strictness inside them rather than dropping it; every AGENTS.md check stays in `New-EmberPackage`. F-TOOLS-2: one `PackageFiles.ps1`; pin manifest encoding explicitly; the shipped-parser fixture covers BOM and no-BOM input. F-TOOLS-6: register the three publisher suites in CTest only with all three conditions: add them to `$missing` in `Invoke-DefaultTests`, update the fake-ctest fixture in test-build-provenance.ps1, and name them in the component text. F-TOOLS-21: split NightlyOperations.ps1 and its test after F-TOOLS-6.
- Verify: test-build-provenance.ps1, test-nightly-publication.ps1, a dry package.
- Saving: five script-scope globals and about 15 lines of glue; about 30 duplicated lines; one test mechanism instead of two; Nightly scripts under about 300 lines each.

#### T3-20 Room host sources and SDK contract (P2 for F-TOOLS-7 and F-TOOLS-10)
- Files: `server/roomhost/build-linux.sh:53-83`, `scripts/build-nightly-room-hosts.sh:4-21`, `CMakeLists.txt:143-583`, `sdk/typescript/src/events.ts`, `client.ts`, `testing.ts`, `signed.ts`, `sdk/typescript/tools/selftest-checks.ts`, `selftest-challonge.ts`, a new `server/ember/protocol/examples/contract.rs`.
- F-TOOLS-10: one `server/roomhost/sources.txt` read by CMake and build-linux.sh (strip CR); in build-nightly-room-hosts.sh, the cleanup trap runs after the copy. F-TOOLS-7: a protocol contract example and an SDK key test. F-TOOLS-8: additive SDK types; renames need a deprecation period. F-TOOLS-9: the TestPlayer timeout as an option, noted in SDK release notes.
- Verify: BridgeSdk, the WSL room host build.
- Saving: one source list instead of two; WSL stops keeping a source tree per Nightly; two of four hand-kept event lists; about 40 lines in the SDK.

#### T3-21 Build time
- Files: `CMakeLists.txt`, `cmake/sf4e_tests.cmake`, `cmake/RunSdkTests.cmake:20`, `cmake/sf4e_embed.cmake`, `cmake/sf4e_fonts.cmake`, `scripts/NightlyOperations.ps1:554`, `scripts/BuildEnvironment.ps1:55`, `scripts/BuildProvenance.ps1`.
- F-TOOLS-11: split the test registrations out of CMakeLists.txt; keep the explicit empty-token and duplicate scans if the parser is rewritten. F-TOOLS-13: an `npm ci` stamp covering node and npm versions, checked against `node_modules/.package-lock.json` (a few seconds). F-TOOLS-15: per-file copy of selection art under the install filter (about a second) and delete the photographs already in build folders once. F-TOOLS-16: trim only the fingerprint at `NightlyOperations.ps1:554`, keeping its receipt, staged-hash and clean-HEAD checks (about 2-3 s). F-TOOLS-17: idempotent `Enter-EmberVcEnvironment`, comparing normalized `VSINSTALLDIR` and `VSCMD_ARG_TGT_ARCH`. F-TOOLS-22: build-time font embedding only.
- Verify: `ctest -N` before and after, one build-current run.
- Saving: root CMakeLists.txt to about 450 lines; a few seconds per test run (npm), about a second per build and 123 MiB of build-folder disk (art), about 2-3 s per Nightly (fingerprint), two vcvarsall runs per Nightly, a few configure seconds (fonts).

#### T3-22 Stale scripts and deploy scripts
- Files: `scripts/build-discord-local.ps1`, `run-package-tests.ps1`, `sign-release-binaries.ps1`, `desktop-sf4e-helper.ps1`, `run-offline-test.ps1`, `CMakePresets.json`, `build-target.json`, `docs/guides/DISCORD.md`, `docs/development/CODE_SIGNING.md`, `server/ember-rooms/deploy/setup.sh`, `idle-close.py`, `ember-rooms-idle.timer`, `server/ember-rooms/src/config.rs`, `server/ember-reports/deploy/setup.sh`, `server/ember-short/deploy/setup.sh`.
- F-TOOLS-12: delete desktop-sf4e-helper.ps1, run-offline-test.ps1 and CMakePresets.json; update DISCORD.md and CODE_SIGNING.md with the script changes. F-TOOLS-18: config subcommands in the ember-rooms binary, run only after its ELF check, with a fake binary in test-setup.sh. F-TOOLS-19: a shared deploy-lib.sh, verified with an `nginx -T` diff on the VPS. F-TOOLS-20: `lone_close_secs` in the supervisor with default 0 and 900 in the VPS config, the supervisor deployed everywhere before the timer is removed, and the exact-timing change accepted explicitly.
- Verify: the setup.sh test scripts; a VPS dry run.
- Saving: about 230, 70, 60 and 250 lines (F-TOOLS-12, -18, -19, -20); about 2,880 Python process starts a day on the VPS.

## 5. Process and release risks

- No channel guard on Stable publication (F-TOOLS-3, T1-19). github-release.ps1 never reads the build target's channel and package-team accepts both. A build from sf4-nightly carries `stagingBridge: true` (a helper that trusts the staging bridge) and, given a new version tag on a nightly commit, passes every github-release check and would be uploaded as Stable Latest. Today only the existing v1.4.1 release stops it.
- Nightly version equals Stable (F-TOOLS-4, T1-20). nightly's CMakeLists.txt says 1.4.1, the same as `release`. The renumber commit that followed every earlier Stable release (1.2.0, 1.3.0, 1.4.0) is missing after 1.4.1. Nightly players still update correctly; a Stable 1.4.1 to Nightly switch is shown as "go back". Bump to 1.5.0.
- A test that checks a test-only copy (F-TEST-2, T1-21). The `stable_voters` test passes whatever production does. Note the adversary's correction: `admission_voters` is already unit-tested in members.rs, and the 3007 test does exercise production tombstone aging.
- Answer types deny unknown fields (F-PROTO-6, T1-12). `ClaimAnswer`, `PrepareAnswer`, `RoomSummary`, `RoomList`, `ConnectionRoom`, `LookupAnswer`, `Standing`, `Board`, `Challenge` and the event envelope are strict in every released helper and in the notifier. Adding a field to an answer breaks every reader built before the change. A new answer field must be gated by the client asking for it in the request, not by a capability the bridge lists. Removing the strictness helps only readers built after the change. `ErrorCode` has no catch-all, so an unknown error code makes an older helper report `http_<status>`.
- F-ROOMS-7 as written would break released clients. Adding a build label to `RoomSummary` breaks every released room browser because the helper parses it strictly. Labels go on the web-only `PublicRoomList` (T3-12).
- F-BRIDGE-15 would break `-Rollback`. The upgrade scripts roll back by running the previous binary on the migrated database. Replacing `discord_*` tables with views makes the old binary's writes fail, so Discord linking breaks on rollback (T3-10). The same constraint rules out reading `rules` as a strict typed struct (F-BRIDGE-8 adjustment) and tightening `required_build_id` (F-PROTO-8, also a partner wire break).
- Match state after a rollback (F-BRIDGE-9, T2-10). `matches.state` and `delivery_state` have no CHECK constraint. A state added by a newer bridge reads as `Failed` on an older one after `-Rollback`, and the match silently stops. The role fallback is not reachable through the bridge because of its CHECK. This is quiet hardening; it stays out of release notes.
- The same-build gate covers Sidecar.dll only (T2-3). The invite proof compares the SHA-256 of Sidecar.dll, not sf4-net.exe. Any change to the Raft wire must add a helper wire version to the compared identity and bump the coordination ALPN. Public rooms depend on the procedural rule that `builds/<build_id>/` stages a room host and its helper from one commit.
- The shared frame version (T2-1). `wire::VERSION` heads IPC frames, QUIC control frames and game UDP packets. An IPC change bumps an IPC-only version, never this one.
- Publisher test move (F-TOOLS-6, T3-19). Moving the publisher suites into CTest weakens provenance unless they are added to the required list in `Invoke-DefaultTests`.

## 6. Rejected, do not redo

Whole findings:
- F-OVH-10: membership snapshot per peer; microseconds per publication. The serialize-once part survives in T3-2.
- F-COORD-11: the authority barrier sends empty appends to voters only, at most about 8 tiny encodes a second; changing the coalesce window is a freshness change (T2-13).
- F-COORD-12: fewer retained logs; 8 are kept on purpose for a slow voter and the snapshot path is less tested.
- F-COORD-15: scratch-chunk copy; a few microseconds, and `resize` replaces it with a memset.
- F-SVC-12: refresh store reads; uncontended, off the actor, and "skip when unchanged" would break time-driven work.
- F-SESSION-14: verification relay; 2-10 KB/s on a local pipe and the format is fixed by NETPLAY_INVARIANTS.
- F-SF4E-16: overlay prefs copy and presentation envelope; negligible, and a dirty flag adds a way to miss an edit.
- F-UI-9: one FitLabel; the 2026-09 review already rejected it because merging changes pixels.
- F-UI-17: installer confirm hash shortcut; weakens the integrity guarantee.
- F-UI-18: updater release parse; runs once per launch, nothing polls.
- F-PROTO-7: custom strict serializer and deserializer; microseconds saved on the signing path for real risk. F-TOUR-3(e) is the same.
- F-TOUR-14: assignments ETag; the body carries the global event cursor and time-based expiry.
- F-TOOLS-14: drop `BUILD_ALWAYS`; the companion reads headers and files outside its folder, so a glob would stage stale binaries.

Rejected parts of otherwise accepted findings:
- F-OVH-1: erasing superseded projections outright (breaks parked `data_update` authorization).
- F-OVH-3 and F-COORD-1: the `RawValue` checkpoint step and compress-once without a binary codec and identity gate.
- F-OVH-6 and F-BRIDGE-6: a bridge-side body cache, a whole-body ETag, and the in-game "unchanged" answer for now. F-UI-13's `since=` request is deferred for the same reason.
- F-OVH-7(b): answering learners from the host's cached claim.
- F-COORD-10: adding new validity rules to apply or install while unifying.
- F-SVC-5 step 2: send ControlRebound only on change.
- F-SESSION-1: hashing a substring of the received text.
- F-SESSION-2: gate first, digest later.
- F-SESSION-5: bumping the shared `wire::VERSION`.
- F-SESSION-10(d): step-count retries converted to milliseconds.
- F-SESSION-11: a clock that stops on passive replicas.
- F-SF4E-8 and F-UI-15: a ping message type, and lengthening the companion keepalive.
- F-SF4E-13: building v1 snapshots from the trace; retiring v1 is a separate DESYNC_V2 decision.
- F-SF4E-14 and F-UI-6: a transparent hash (C++20) and a string_view index over all 14 catalogs.
- F-UI-1: moving the shell's own screens into a panel now.
- F-UI-7: the revision-keyed early return and the glyph dirty flag.
- F-BRIDGE-7: caching the player listing.
- F-BRIDGE-8: strict typed reads of `rules` and `metadata`.
- F-ROOMS-6: the rankings fold change.
- F-ROOMS-7: a label field on `RoomSummary`.
- F-PROTO-8: "a non-ASCII build makes a match unplayable" (false; the bridge never compares `required_build_id`) and its tightening.
- F-PROTO-15: a longer EventSub keepalive.
- F-TEST-5: converting negative windows to condition waits.
- F-TOOLS-22: replacing `-U*_DIR` with a path heuristic (it is the cross-checkout guard).

## 7. Index

Columns: reviewer severity (Rev), adversary verdict, final severity (Final), canonical finding if merged (Dup of), work item (Item). C = CONFIRMED, P = PLAUSIBLE, R = REJECTED, D = DUPLICATE.

| Id | Title | Category | Rev | Verdict | Final | Dup of | Item |
|---|---|---|---|---|---|---|---|
| F-OVH-1 | Effect journal keeps superseded projections to budget | streaming | P1 | P | P1 | canonical | T1-1 |
| F-OVH-2 | Helper parses checkpoint into Value twice | overhead | P2 | C | P2 | F-SVC-4 | T1-3 |
| F-OVH-3 | Leader encodes same Raft entry per follower | overhead | P2 | C | P2 | F-COORD-1 | T1-2 |
| F-OVH-4 | Checkpoint crosses pipe as base64 JSON events | overhead | P2 | P | P3 | F-SVC-14 | T2-1 |
| F-OVH-5 | Room snapshot parsed three to four times | overhead | P3 | C | P3 | F-SESSION-1 | T1-7 |
| F-OVH-6 | Rooms list and board refetched uncompressed | overhead | P2 | P | P2 | canonical | T1-9 |
| F-OVH-7 | Followers request authority claim every second | overhead | P3 | P | P3 | no | T2-13 |
| F-OVH-8 | Bridge rewrites every room every 5 s | overhead | P3 | P | P3 | F-BRIDGE-1 | T1-8 |
| F-OVH-9 | Pipe thread and actor tick at 500 Hz | overhead | P3 | P | P3 | F-SESSION-6 | T2-4 |
| F-OVH-10 | Membership snapshot serialized once per peer | overhead | P3 | R | P3 | no | T3-2 |
| F-COORD-1 | Leader compresses each append per follower | overhead | P1 | P | P1 host, P2 private | canonical | T1-2, T2-3 |
| F-COORD-2 | base64 and JSON wrap binary bodies | overhead | P2 | P | P2 | part of F-COORD-1 | T2-3 |
| F-COORD-3 | Checkpoint string copied four to six times | overhead | P2 | C | P2 | part of F-COORD-1 | T1-2 |
| F-COORD-4 | build_snapshot holds store mutex while encoding | overhead | P2 | C | P2 | no | T1-4 |
| F-COORD-5 | Two Value walks for a few fields | overhead | P2 | D | P2 | F-SVC-4 | T1-3 |
| F-COORD-6 | Leader's helper echoes own commit back | overhead | P2 | P | P3 | part of F-SVC-14 | T2-2 |
| F-COORD-7 | Three-layer pass-through and telescoping constructors | judo | P2 | C | P3 | no | T3-1 |
| F-COORD-8 | coordination.rs and recovery.rs mix concerns | size | P2 | C | P3 | no | T3-1 |
| F-COORD-9 | RPC method string matched in five tables | boundary | P2 | C | P3 | no | T3-1 |
| F-COORD-10 | Proposal validity rules written three times | spaghetti | P3 | P | P3 | no | T3-1 |
| F-COORD-11 | Authority polling runs codec on heartbeats | overhead | P3 | R | P3 | no | none |
| F-COORD-12 | Lagging follower replays superseded entries | streaming | P3 | R | P3 | no | none |
| F-COORD-13 | transport.rs guard repeated, errors by string | judo | P3 | C | P3 | no | T3-1 |
| F-COORD-14 | invite.rs decodes header and prefixes twice | judo | P3 | C | P3 | no | T3-1 |
| F-COORD-15 | Body framing copies through scratch chunk | overhead | P3 | R | P3 | no | none |
| F-SVC-1 | Per-room state is loose Actor fields | structural | P1 | P | P2 | no | T2-9 |
| F-SVC-2 | mod.rs mixes types, handler and loop | size | P1 | C | P3 | no | T3-2 |
| F-SVC-3 | Two-tier dispatch with dead Leave arm | judo | P2 | C | P3 | no | T3-2 |
| F-SVC-4 | Checkpoint parsed into Value twice per member | overhead | P1 | P | P2 | canonical | T1-3 |
| F-SVC-5 | ControlRebound resent each second, dead marker | overhead | P2 | P | P3 | no | T3-2 |
| F-SVC-6 | Membership publication serialized per peer, repeated | overhead | P2 | P | P3 | no | T3-2 |
| F-SVC-7 | Pump locks Raft store every tick | overhead | P2 | C | P3 | no | T1-4 |
| F-SVC-8 | Native messages copied and parsed per hop | overhead | P2 | P | P3 | no | T3-2 |
| F-SVC-9 | Admission equality by serializing to JSON | judo | P3 | C | P3 | no | T3-2 |
| F-SVC-10 | Copy-pasted control sends and marker resets | spaghetti | P2 | C | P3 | no | T3-2 |
| F-SVC-11 | Endpoint identities compared as strings | boundary | P2 | C | P3 | no | T3-2 |
| F-SVC-12 | Per-second refresh does many store reads | overhead | P3 | R | P3 | no | none |
| F-SVC-13 | Small duplications, string error classification | spaghetti | P3 | C | P3 | no | T3-2 |
| F-SVC-14 | Checkpoint export base64-encodes every commit | overhead | P3 | P | P3 | copy half: F-COORD-3 | T2-1, T1-2 |
| F-SESSION-1 | Room message reparsed at every hop | judo | P1 | C | P1 | canonical | T1-7 |
| F-SESSION-2 | Parked effect reparsed and rehashed per poll | overhead | P1 | C | P1 | no | T1-5 |
| F-SESSION-3 | base64 decode with linear alphabet search | overhead | P2 | C | P2 | no | T1-6 |
| F-SESSION-4 | Two SHA-256 implementations, CNG per call | judo | P2 | C | P3 | no | T3-13 |
| F-SESSION-5 | Payloads cross pipe as escaped JSON | streaming | P2 | P | P3 | F-SVC-14 | T2-1 |
| F-SESSION-6 | Windows pipe worker polls every 2 ms | overhead | P2 | C | P2 | canonical | T2-4 |
| F-SESSION-7 | IrohRoom::Poll pumped four times per tick | structural | P2 | P | P3 | no | T3-13 |
| F-SESSION-8 | IrohRoom fields reset by hand | judo | P2 | C | P3 | no | T3-13 |
| F-SESSION-9 | Dense one-line style hides control flow | spaghetti | P3 | C | P3 | partly F-SESSION-8 | T3-13 |
| F-SESSION-10 | SessionClient mixes stepping, verification, legacy sends | size | P2 | C, (d) R | P3 | no | T3-13 |
| F-SESSION-11 | Room timestamps change meaning when paused | judo | P2 | P | P3 | no | T3-13 |
| F-SESSION-12 | RoomModel files have uncut seams | size | P3 | C | P3 | no | T3-13 |
| F-SESSION-13 | Broadcast deep-copies snapshot per member | overhead | P2 | C (1), P (2) | P2 | no | T2-7 |
| F-SESSION-14 | Verification relay is the recurring send | overhead | P3 | R | P3 | F-SESSION-1, F-SESSION-5 | none |
| F-SESSION-15 | IrohRoom::Game copies snapshot per tick | boundary | P3 | C | P3 | no | T3-13 |
| F-SF4E-1 | GGPO frame step exists three times | judo | P1 | P | P2 | no | T2-17 |
| F-SF4E-2 | Game thread reads its own published snapshot | boundary | P1 | P | P2 | no | T2-19 |
| F-SF4E-3 | Session role and drain re-derived everywhere | spaghetti | P1 | P | P2 | no | T2-18 |
| F-SF4E-4 | Dispatch is a 420-line if-chain | structural | P1 | P | P2 | no | T2-20 |
| F-SF4E-5 | Room deep-copied every tick and frame | overhead | P2 | C | P2 | canonical | T1-16 |
| F-SF4E-6 | Sound metadata in pointer-keyed maps | judo | P2 | P | P3 | no | T3-14 |
| F-SF4E-7 | Every save churns trackedKeys set nodes | overhead | P2 | C | P2 | no | T1-14 |
| F-SF4E-8 | Discord presence rebuilt and resent each second | streaming | P2 | P | P2 | canonical | T2-8 |
| F-SF4E-9 | Menu ticks re-read devices, settings, pads | overhead | P2 | P | P2 | no | T1-15 |
| F-SF4E-10 | Runtime is a 120-field object | structural | P2 | C | P3 | no | T3-14 |
| F-SF4E-11 | Two session starts duplicate the reset | spaghetti | P2 | P | P2 | no | T2-21 |
| F-SF4E-12 | Seams for four large hook files | size | P2 | P | P3 | no | T3-14 |
| F-SF4E-13 | Battle-flow globals listed in five places | judo | P3 | P | P3 | no | T3-14 |
| F-SF4E-14 | loc::T allocates every tick | overhead | P3 | P | P3 | F-UI-6 | T1-18 |
| F-SF4E-15 | Dimps accessors out of line, no LTCG | size | P3 | C | P3 | no | T3-14 |
| F-SF4E-16 | Small per-frame overlay copies | overhead | P3 | R | P3 | no | none |
| F-UI-1 | Screen dispatch is 26 string checks | judo | P1 | P | P2 | canonical | T2-22 |
| F-UI-2 | On/off rows enumerated by hand | judo | P2 | C | P3 | no | T3-15 |
| F-UI-3 | Row ids parsed back with stoi | boundary | P2 | P | P3 | no | T3-15 |
| F-UI-4 | ShellView copies snapshot every drawn frame | overhead | P2 | P | P3 | F-SF4E-5 | T1-16 |
| F-UI-5 | ShellAction and RuntimeCommand duplicate fields | boundary | P2 | C | P3 | no | T3-15 |
| F-UI-6 | loc::T allocates a string per lookup | overhead | P2 | P | P3 | canonical | T1-18 |
| F-UI-7 | Per-frame bookkeeping recomputes unchanged state | overhead | P2 | P | P3 | no | T1-17 |
| F-UI-8 | Chat log formats all lines per frame | overhead | P3 | P | P3 | no | T3-15 |
| F-UI-9 | FitLabel re-implemented seven times | spaghetti | P3 | R | P3 | no | none |
| F-UI-10 | Shell, RoomPanel, TrainingMeter seams | size | P2 | P | P3 | F-UI-1 (RoomEntries) | T3-15, T2-22 |
| F-UI-11 | HUDs redo layout every frame | overhead | P3 | P | P3 | no | T3-15 |
| F-UI-12 | Image cache keys built per request | overhead | P3 | P | P3 | no | T3-15 |
| F-UI-13 | Public rooms sorted twice, fetched every 15 s | streaming | P3 | P | P3 | F-OVH-6 | T1-9 |
| F-UI-14 | Launcher link kinds in four cascades | spaghetti | P2 | C | P3 | no | T3-15 |
| F-UI-15 | Discord companion loop re-serializes state | overhead | P3 | P | P3 | F-SF4E-8 | T2-8 |
| F-UI-16 | Hidden globals couple menu and training | structural | P3 | P | P3 | no | T3-15 |
| F-UI-17 | Installer confirm pass rehashes everything | overhead | P3 | R | P3 | no | none |
| F-UI-18 | Updater does not poll; parse waste | overhead | P3 | R | P3 | no | none |
| F-BRIDGE-1 | Rooms poll rewrites rows and fsyncs | overhead | P1 | C | P1 | canonical | T1-8 |
| F-BRIDGE-2 | New reqwest client per outbound call | overhead | P2 | C | P3 | no | T3-8 |
| F-BRIDGE-3 | Delivery workers share one notify_one | overhead | P2 | P | P3 | no | T1-10 |
| F-BRIDGE-4 | One connection serialises reads behind writes | structural | P2 | P | P3 | no | T2-11 |
| F-BRIDGE-5 | committed() called by hand at 34 sites | judo | P3 | P | P3 | poll part: F-BRIDGE-1 | T2-12 |
| F-BRIDGE-6 | Public JSON uncompressed, board re-serialised | overhead | P2 | P | P3 | F-OVH-6 | T1-9 |
| F-BRIDGE-7 | Player listing double-serialised | overhead | P3 | P | P3 | no | T3-8 |
| F-BRIDGE-8 | JSON columns read with silent fallbacks | judo | P2 | P | P3 | no | T3-9 |
| F-BRIDGE-9 | Unknown stored enums become real values | boundary | P2 | P | P2 state, P3 role | no | T2-10 |
| F-BRIDGE-10 | matches.rs mixes routes and domain ops | size | P2 | C | P3 | no | T3-9 |
| F-BRIDGE-11 | lobbies.rs engine, model, routes together | size | P2 | C | P3 | no | T3-9 |
| F-BRIDGE-12 | Service write routes repeat one pipeline | judo | P2 | P | P3 | no | T3-9 |
| F-BRIDGE-13 | Match score computed four ways | spaghetti | P3 | P | P3 | no | T3-8 |
| F-BRIDGE-14 | BluMint status wired into ledger bump | spaghetti | P3 | C | P3 | no | T3-8 |
| F-BRIDGE-15 | Discord and site sign-ins use parallel tables | judo | P3 | P | P3 | no | T3-10 |
| F-BRIDGE-16 | Responses built as patched json! values | boundary | P3 | P | P3 | no | T3-9 |
| F-ROOMS-1 | Full room snapshot every 5 s, rewritten | overhead | P1 | P | P2 | F-BRIDGE-1 (bridge part) | T1-8, T2-6 |
| F-ROOMS-2 | Occupied idle room host ticks at 62 Hz | overhead | P2 | P | P2 | no | T2-5 |
| F-ROOMS-3 | Report does five jobs, gate bypassed | structural | P2 | P | P3 | no | T3-11 |
| F-ROOMS-4 | Supervisor decodes status into Value twice | judo | P2 | C | P3 | no | T3-11 |
| F-ROOMS-5 | Supervisor task state in loose parameters | structural | P2 | C | P3 | no | T3-11 |
| F-ROOMS-6 | rooms.json refetched, page rebuilt each time | overhead | P2 | P | P3 | F-OVH-6 | T1-9 |
| F-ROOMS-7 | Release data copy-pasted in site scripts | spaghetti | P3 | P | P3 | no | T3-12 |
| F-ROOMS-8 | Status-line size limits disagree | boundary | P3 | C | P3 | no | T3-11 |
| F-ROOMS-9 | Public JSON proxies probably uncompressed | overhead | P3 | P | P3 | F-OVH-6 | T1-9 |
| F-ROOMS-10 | Small deletions in reports and short | judo | P3 | P | P3 | no | T3-11 |
| F-PROTO-1 | Notifier scans unpruned inbox every second | overhead | P2 | C | P2 | no | T1-11 |
| F-PROTO-2 | Brackets worker wakes on fixed 60 s | overhead | P3 | P | P3 | no | T2-16 |
| F-PROTO-3 | tournament.rs holds wire types and engine | size | P2 | P | P3 | no | T3-6 |
| F-PROTO-4 | Same-attempt test hand-written three times | judo | P2 | C | P3 | no | T3-6 |
| F-PROTO-5 | Event data untyped, read by string keys | boundary | P2 | P | P2 | no | T2-15 |
| F-PROTO-6 | Answer types deny unknown fields | boundary | P2 | C | P2 | canonical | T1-12 |
| F-PROTO-7 | Strict JSON path builds extra trees | overhead | P3 | R | P3 | canonical | none |
| F-PROTO-8 | Field validation duplicated with drifting rules | spaghetti | P2 | R central, P dedup | P3 | no | T3-6 |
| F-PROTO-9 | Signed objects re-implement sign and verify | judo | P3 | C | P3 | no | T3-6 |
| F-PROTO-10 | Tri-state options carrying two states | boundary | P3 | C | P3 | no | T3-6 |
| F-PROTO-11 | Wire enums hand-write name tables | spaghetti | P3 | P | P3 | no | T3-6 |
| F-PROTO-12 | rooms.rs mixes player and provider APIs | structural | P3 | C | P3 | no | T3-6 |
| F-PROTO-13 | Claim answer has no unchanged form | streaming | P3 | P | P3 | F-TOUR-5 | T2-14 |
| F-PROTO-14 | Brackets and notifier copy bridge client | spaghetti | P3 | C | P3 | no | T3-7 |
| F-PROTO-15 | Site and EventSub answers parsed twice | overhead | P3 | C | P3 | no | T3-7 |
| F-TOUR-1 | Identity jobs wait behind HTTP semaphore | structural | P2 | C | P2 | canonical | T1-13 |
| F-TOUR-2 | Authenticated bridge call hand-rolled repeatedly | judo | P2 | C | P3 | no | T3-3 |
| F-TOUR-3 | Bridge answers pass through five trees | boundary | P2 | P | P3 | (e): F-PROTO-7 | T3-3 |
| F-TOUR-4 | Failure strings branched on by equality | boundary | P2 | P | P3 | no | T3-3 |
| F-TOUR-5 | Claim renewal costs two HTTPS round trips | overhead | P2 | P | P3 | canonical | T2-14 |
| F-TOUR-6 | Each frame written with four writes | overhead | P2 | C | P3 | no | T3-4 |
| F-TOUR-7 | stdio helper re-frames through a duplex | judo | P2 | C | P3 | no | T3-4 |
| F-TOUR-8 | Spool resend retries every report on outage | overhead | P2 | P | P3 | no | T3-5 |
| F-TOUR-9 | Every answer carries full identity status | overhead | P3 | C | P3 | no | T3-4 |
| F-TOUR-10 | Four temp-file writers, different durability | spaghetti | P3 | P | P3 | no | T3-5 |
| F-TOUR-11 | Request enum routed by string prefix | spaghetti | P3 | C | P3 | part of F-TOUR-1 | T1-13 |
| F-TOUR-12 | public_room.rs wrapper and duplicate connects | judo | P3 | C | P3 | no | T3-5 |
| F-TOUR-13 | Each request clones whole bridge list | overhead | P3 | C | P3 | no | T3-3 |
| F-TOUR-14 | Assignment poll downloads full list | streaming | P3 | R | P3 | no | none |
| F-TEST-1 | service/tests.rs is a 7384-line catch-all | size | P1 | P | P2 | no | T3-17 |
| F-TEST-2 | Tests check a test-only voter copy | boundary | P1 | P | P2 | no | T1-21 |
| F-TEST-3 | Loopback bridge harness built five times | structural | P1 | C | P2 | canonical | T3-16 |
| F-TEST-4 | Wait-until helpers and silent poll fall-through | structural | P2 | C | P2 | no | T3-16 |
| F-TEST-5 | Negative assertions as fixed sleeps | structural | P3 | P | P3 | no | T3-18 |
| F-TEST-6 | Hand-rolled HTTP stand-ins in sf4-net tests | structural | P2 | P | P3 | no | T3-16 |
| F-TEST-7 | test_actor repeats the Actor literal | structural | P2 | C | P3 | no | T3-17 |
| F-TEST-8 | Session server test has unnamed main scenario | size | P2 | C | P2 | no | T3-17 |
| F-TEST-9 | Giant main bodies in two UI tests | size | P2 | P | P3 | no | T3-17 |
| F-TEST-10 | Tests redefine CHECK with four meanings | spaghetti | P2 | C | P3 | no | T3-18 |
| F-TEST-11 | blumint_reports.rs mixes two halves | size | P2 | C | P3 | no | T3-18 |
| F-TEST-12 | Bridge tests seed schema with copied SQL | boundary | P2 | C | P3 | no | T3-18 |
| F-TEST-13 | Rust suites opaque in CTest, no labels | structural | P3 | C | P3 | no | T3-18 |
| F-TEST-14 | Bare timeout wrappers in tests.rs | spaghetti | P3 | C | P3 | no | T3-17 |
| F-TOOLS-1 | Packaging scripts return through script variables | boundary | P2 | C | P3 | no | T3-19 |
| F-TOOLS-2 | Manifest and sidecar primitives copy-pasted | judo | P2 | P | P3 | no | T3-19 |
| F-TOOLS-3 | Stable publication has no channel guard | boundary | P2 | C | P2 | no | T1-19 |
| F-TOOLS-4 | Nightly version not one minor ahead | boundary | P3 | C | P3 | no | T1-20 |
| F-TOOLS-5 | Stable and Nightly use two publishers | judo | P2 | P | P2 | no | T2-23 |
| F-TOOLS-6 | Publisher tests run outside CTest | structural | P2 | P | P3 | no | T3-19 |
| F-TOOLS-7 | SDK wire types hand-synced and drifted | boundary | P2 | C | P2 | no | T3-20 |
| F-TOOLS-8 | SDK typed contract stops at central objects | boundary | P2 | P | P3 | no | T3-20 |
| F-TOOLS-9 | TestPlayer re-implements HTTP without safeguards | judo | P3 | P | P3 | no | T3-20 |
| F-TOOLS-10 | Linux room host source list duplicates CMake | spaghetti | P2 | C | P2 | no | T3-20 |
| F-TOOLS-11 | CMakeLists.txt interleaves tests and targets | size | P3 | P | P3 | no | T3-21 |
| F-TOOLS-12 | Stale scripts and configs | spaghetti | P3 | C | P3 | no | T3-22 |
| F-TOOLS-13 | BridgeSdk reinstalls node_modules each run | overhead | P2 | P | P3 | no | T3-21 |
| F-TOOLS-14 | Two ExternalProjects always rebuild | overhead | P3 | R | P3 | no | none |
| F-TOOLS-15 | Every build compares unshipped selection art | overhead | P3 | P | P3 | no | T3-21 |
| F-TOOLS-16 | Fingerprint recomputed seven times per Nightly | overhead | P3 | C | P3 | no | T3-21 |
| F-TOOLS-17 | vcvarsall runs three times, PATH grows | overhead | P3 | C | P3 | no | T3-21 |
| F-TOOLS-18 | setup.sh re-implements supervisor config | judo | P3 | P | P3 | no | T3-22 |
| F-TOOLS-19 | Three deploy scripts, three rollback models | spaghetti | P3 | P | P3 | no | T3-22 |
| F-TOOLS-20 | idle-close.py polls supervisor every 30 s | judo | P3 | P | P3 | no | T3-22 |
| F-TOOLS-21 | NightlyOperations and its test are linear | size | P3 | C | P3 | no | T3-19 |
| F-TOOLS-22 | Configure-time fonts and package re-find | overhead | P3 | P fonts, R -U | P3 | no | T3-21 |

## 8. Not verified

Nothing was built, tested, benchmarked or deployed for this review, apart from the one fingerprint timing noted at the end, and no state was changed in the repository. Every number above is an estimate from reading code. The specific gaps the reviewers and adversaries named:

- Raft and pipe volumes: the real zlib ratio on checkpoints (8x assumed, 5-12x plausible) and real commit rates per room (0.07 / 0.25 / 0.5 per second assumed). RoomHostBench is network-free and never measured the per-follower encode; it should print encoded and compressed append sizes before T1-1 and T1-2.
- All millisecond figures for serde_json, nlohmann, zlib, base64 and SHA-256 work; the number of ticks an effect stays parked; whether any UI consumer diffs consecutive room snapshots (relevant to T1-1).
- The live VPS nginx configuration: whether http-level gzip already covers JSON, and the actual WAL bytes and fsync count per rooms poll on the bridge.
- The BluMint match board's page poll cadence (it lives only on the VPS).
- The process timer resolution inside SSFIV.exe, which decides whether the Windows pipe waits are about 2 ms or about 15.6 ms (T2-4).
- mio's handling of overlapped named-pipe writes, on which F-TOUR-6's round-trip count rests.
- The engine and GGPO fork internals: IDA was down, so whether any memento record reads pads (T2-17), the real count of nested ClearKey calls per save (T1-14), and the fork's in-rollback poll behavior were not checked.
- Whether a fresh install stays on the settings.json miss path for long (T1-15).
- Per-tick CPU of an occupied idle room host and any time-driven work outside `HasDueTimerTransition` (T2-5); the soak monitor has to answer it.
- Whether every bridge writer that makes delivery work due calls `committed()`, and whether any ops script writes the bridge database directly (T1-10, T2-12).
- Whether `scripts/build-current.ps1` always relinks Sidecar.dll, which decides how likely a same-hash, different-helper package is (T2-3).
- Whether SQLite uses the proposed notifier partial index (needs `EXPLAIN QUERY PLAN`) and real inbox row counts on the deployed notifier.
- Whether every room mutation moves `room.revision` on peer-hosted and server-owned rooms (why T1-17 does not key on it).
- Whether the rewritten 3007 test can reach a matching committed revision, and Cargo's behavior with a self-referencing dev-dependency with features (T1-21, T3-16).
- Whether server1's Docker deployment runs the idle-close timer outside the repository's units (T3-22).
- Timings of npm ci, MSBuild no-op builds, vcvarsall and configure; only `Get-SourceFingerprint` was timed (2.0 s warm, 3.0 s cold).
