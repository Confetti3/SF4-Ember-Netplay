# Runtime overhead and streaming audit (whole tree, nightly b9ce0e31)

Scope: recurring sends and serialization hot paths across `rust/sf4-net`, `src/`, `server/`,
`server/ember-short/deploy/site`. Read-only; no measurement was run. Sizes are estimated from the
structs and limits named in each row; where a number depends on a compression ratio or a commit
rate, the assumption is stated.

## 0. The owner's "large JSON file sent every 15 seconds"

Best match: **the public rooms list, refreshed every 15 s**. It exists twice:

- Web: `server/ember-short/deploy/site/assets/rooms.js:9` (`REFRESH_MS = 15000`), re-armed after each
  fetch at `:265-268`, stopped while the tab is hidden (`:278-280`). It fetches `/rooms.json`
  (nginx `ember-short-locations.conf:83-88`, cached 5 s) which proxies the bridge's
  `GET /v1/rooms/public` (`server/ember/bridge/src/routes/rooms/mod.rs:506`, `PublicRoomList` at `:496`).
- In game: `src/ui/PublicRoomsPanel.hxx:40` (`AutoRefreshSeconds = 15`), scheduled at
  `PublicRoomsPanel.cxx:220,256`, while the Public rooms screen is open and the player is idle. The
  helper calls `GET /v1/rooms?build_id=...&detail=1` (`rust/sf4-net/src/tournament/rooms.rs:95-97,121-130`),
  turns the list into a `serde_json::Value` (`rooms.rs:111`) and sends it to the game as
  `Event::Tournament.data` over the pipe, where nlohmann parses it again.

Payload: up to 100 `RoomSummary` with details (`server/ember/protocol/src/rooms.rs:202-239`):
`room_id` (32 hex), `name` (<= 64), `build_id`, counts, `region`, `created_at`, `host_name` (<= 32),
`fighters` (one number per member), `locked`, `set_format`, `rotation`. About 250-350 bytes per room:
~13 KB at 45 rooms (the VPS's configured capacity), ~33 KB at 100. It is the only JSON *document*
that is fetched whole on a fixed 15 s timer. It is not gzip-compressed (the only `gzip_types` in the
repo is the `/assets/` block, `ember-short-locations.conf:173`, which omits `application/json`), has no
ETag, and the page fetches with `cache: 'no-store'` (`rooms.js:243`), so every poll is a full 200.

What it is not: the genuinely large JSON in Ember, the room recovery checkpoint (~280-460 KB, see
F-OVH-1), is sent per committed room mutation, not on a timer. It does have a 15 s *failure-mode*
loop: a proposal that has not committed after 15 s is dropped (`src/session/IrohRoomCheckpoint.cxx:296-304`)
and `RoomRecoveryRuntime::Tick` hands the whole pending proposal to the helper again
(`src/session/RoomRecoveryRuntime.hxx:135-138`), and the helper's own export to the game retries
after `CHECKPOINT_TRANSFER_TIMEOUT` = 15 s (`rust/sf4-net/src/service/mod.rs:74`,
`checkpoints.rs:368-411`). If she saw a large JSON every 15 s in a log while a room was unhealthy,
that is the loop. In normal operation, the 15 s item is the rooms list. To confirm without a build:
the VPS nginx access log shows `/rooms.json` per web viewer, and the bridge log shows
`GET /v1/rooms?build_id=...&detail=1` per in-game viewer, each at a 15 s cadence.

Other 15 s constants checked and ruled out as sends: `LEASE` (`service/latency.rs:49`) is a local
subscription TTL renewed by a ~110-byte `latency_watch` pipe command every 5 s
(`src/session/MemberLatency.hxx:38`); iroh's `PATH_MAX_IDLE_TIMEOUT` 15 s; `DepartureGraceMs`,
`OwedAbort::WaitMs`, `kNetplayWindowMs` (a log flush, `sf4e__Game__Battle__System__Ggpo.cxx:69,604`),
`TrainingCall::ReadyWindowMs`, `CreatedRules::DeadlineMs` are timers. Neither
`docs/development/SERVER_TRAFFIC.md` nor `C:\Users\Kate\Desktop\sf4\VPS_CLIENT_TRAFFIC.md` names a 15 s flow
(they list the 5 s supervisor poll, the 5 s nginx cache and the 10 min invite PUT).

## 1. Fan-out model used in every row

- Private room, N members: the leader is a voter; voters = 1 for N <= 2, 3 for N = 3-4, 5 for N >= 5
  (`recovery.rs:888-894`, `MAX_VOTERS = 5`); every other member is a learner. Raft replicates every
  entry to every follower (voters and learners). Each non-leader member also holds one control
  connection and one coordination connection to the leader.
- Public (server-owned) room, N members: the VPS room host's helper is the sole voter
  (`seats.rs:139-141`, `coordination.rs:1137-1144`); all N members are learners. So all replication
  leaves the VPS.
- Latency board (only while open): one QUIC connection per direction per member pair, N(N-1) in the
  room (`service/latency.rs:1-9`).
- Games: fighters pairwise; P1 to each spectator. Gameplay is out of scope (sub-second).
- Commit rate assumption for "per room" bytes/s of event-driven items: r = 0.07/s (2 members playing
  sets: Ready, start, result, ack per ~60-90 s game), 0.25/s (8 members, 2-3 tables), 0.5/s (16
  members, four tables plus chat). A quiet room commits nothing: `AdvanceCustomRoom` builds a
  candidate only at a due timer and drops no-op candidates (`sf4e__SessionServer__Rooms.cxx:227-272`).
- Compression assumption: `CompressedJsonWire` uses zlib `Compression::fast()`
  (`coordination.rs:79-98`). Recipient projections are near-duplicates and sit within deflate's 32 KB
  window, so 8x is assumed (plausible range 5-12x). Base64 adds 4/3. Every Raft size below scales
  linearly with that ratio; RoomHostBench should print the compressed append size to pin it.

## 2. Inventory of recurring sends (interval >= 1 s), plus the per-commit stream they ride on

P = encoded `SessionProposal` per commit (what Raft replicates and the pipe exports). From the code:
the checkpoint's `effect_journal` sits at its 256 KB budget in steady state in every room size (see
F-OVH-1), plus room state (~15-45 KB) plus this commit's effects. Estimated P ~= 280 KB (2 members),
320 KB (8), 460 KB (16), consistent with the 320 KiB comment at `coordination.rs:1118-1121` and the
460 KB in `docs/perf/ROOM_PERF_BASELINE.md`. W = Raft wire bytes per follower per commit =
P x ~1.15 (JSON-escaping the checkpoint string) / 8 x 4/3 ~= 53 / 61 / 88 KB.

| # | Where | Payload (dominant fields) | Est. bytes | Interval | Fan-out | Transport | B/s per room at 2 / 8 / 16 members |
|---|---|---|---|---|---|---|---|
| A1 | `coordination.rs:1041-1053` (append), `try_get_log_entries` `:716-728`, apply `:797-899` | `AppendEntriesRequest` carrying one `Proposal` whose `checkpoint` String is the whole encoded `SessionProposal` (`coordination.rs:163-178`) | W ~= 53 / 61 / 88 KB | per commit (event) | every follower: private N-1, public N | QUIC RPC, JSON{zlib+base64(JSON)} | private: 3.7 K / 107 K / 660 K; public (VPS egress): 7.4 K / 122 K / 704 K |
| A2 | `checkpoints.rs:265-310,464-532`; `recovery.rs:24-25` | `Event::CheckpointBegin/Chunk/End`; each chunk 16 KiB base64 in JSON, window 4 | P x 4/3 + ~30 x 250 B = 373 / 427 / 613 KB | per commit | every member's own helper -> game | named pipe, JSON | per member: 26 K / 107 K / 306 K (local; room-wide x N) |
| A3 | `sf4e__SessionServer__Rooms.cxx:391-433` (`BroadcastRoomState`) | `RoomSnapshotMessage` per member (~13 KB without chat at 16, chat ~2/3 when sent) + `SessionDataUpdate` per member | baseline doc: 164 KB/commit at 16 | per commit | leader -> every member | QUIC control stream | ~0.8 K / 21 K / 82 K |
| B1 | `service/mod.rs:1505-1508` -> `refresh.rs:106-163` -> `recovery.rs:407-437` -> `coordination.rs:1186-1220` | follower: RPC `authority` (1-byte body + 41-byte header, `coordination_iroh.rs:741-757`); answer `AuthorityClaim` (`coordination.rs:313-327`: 4 u64 + `voters` + `members` sets, ~550 B at 16) | ~0.7 KB round trip incl. QUIC | 1 s | every non-leader member -> leader (public: every member -> VPS host) | QUIC RPC | private: 0.7 K / 4.5 K / 11 K; public: 1.4 K / 5.2 K / 12 K |
| B2 | same path, leader side: `coordination.rs:1233-1273` (coalesce 500 ms; host 2.5 s) -> `ensure_linearizable` (openraft `raft_core.rs:284-344`, voters only) | empty `AppendEntriesRequest` in `CompressedJsonWire` (its `digest: [u8;32]` serializes as a 32-number JSON array) | ~0.55 KB round trip | up to 2/s (1 Hz own check + followers' claims, 500 ms coalesce) | leader -> each other voter | QUIC RPC | 0 (1 voter) / 4.4 K / 4.4 K; public: 0 |
| B3 | `coordination.rs:1124` `heartbeat_interval: 9_000`; openraft `replication/mod.rs:229-234,392-395` | heartbeat = AppendEntries with `entries: []` (verified: `Data::Heartbeat` -> `LogIdRange(m, m)`); also sent at once after each commit to carry `leader_commit` (`replication/mod.rs:671-685`) | ~0.55 KB | 9 s | leader -> every follower incl. learners | QUIC RPC | 0.06 K / 0.43 K / 0.9 K |
| B4 | iroh 1.1.0 `socket.rs:109` `HEARTBEAT_INTERVAL = 5 s`, applied in `endpoint/quic.rs:156-157`; latency transport does not override it (`service/latency/endpoint.rs:53-66`) | QUIC PING + ACK per idle connection (and per extra path) | ~120 B / 5 s / connection | 5 s | control + coordination (2(N-1)) + latency board N(N-1) while open + game links | UDP/QUIC | board open: 0.1 K / 1.7 K / 6.5 K; closed: ~0.1 / 0.3 / 0.7 K |
| B5 | `coordination.rs:1135-1144` snapshot policy `LogsSinceLast(3 host / 8 others)` | `encode_snapshot` (`:423-443`): whole `Machine` JSON + zlib fast; install to a lagging learner in 64 KiB base64 RPCs (`:1145-1149`) | ~W per install | every 3 / 8 commits (build, CPU only); install only when a learner is behind | local; lagging learner | CPU; QUIC RPC | install-only |
| C1 | `service/mod.rs:1529-1537` | `Event::Statistics` (`protocol.rs:388-400`): 8 u64 + 64-hex peer + route string | ~330 B | 1 s | per game slot (fighter 1; P1 host 1 + spectators) | pipe | per member 0.33 K x slots |
| C2 | `service/mod.rs:1517-1521` | `Event::HelperLoad` (`protocol.rs:407-416`) | ~170 B | 1 s in a game, 5 s with only a room | per member | pipe | per member 0.17 K / 0.03 K idle |
| C3 | `service/latency.rs:510-540,564-585`, `protocol.rs:317-322` | `Event::MemberLatency`, rows `MemberLatencyRow` (`protocol.rs:458-520`), ~62 B/row + ~140 B envelope (`room` is a 16-number array) | 0.2 / 0.57 / 1.07 KB | <= 1 s, on change (RTT ms jitter makes it most seconds) | per member, board open | pipe | per member 0.2 K / 0.57 K / 1.07 K |
| C4 | `src/session/MemberLatency.hxx:38`, `IrohRoom.cxx:400` | `latency_watch` command | ~110 B | 5 s | per member, board open | pipe (game -> helper) | 0.02 K per member |
| C5 | `service/mod.rs:1513-1516`; `refresh.rs:313-335` | `NetworkSummary`, `CoordinationState` | ~0.3 KB | checked 1 s, sent on change only | per member | pipe | ~0 |
| C6 | `service/stall.rs:45` | watchdog reads two atomics | 0 | 250 ms | per helper | none (CPU) | 0 |
| D1 | `PublicRoomsPanel.hxx:40`; `tournament/rooms.rs:95-130` | `RoomList` with details (`protocol/src/rooms.rs:202,303`) | ~300 B/room: 13 KB at 45 rooms, 33 KB at 100 | 15 s while the screen is open and idle | per viewing player | HTTPS to bridge, then pipe (`Event::Tournament`) | per viewer 0.9-2.2 K (not per room) |
| D2 | `rooms.js:9,265-268,278-280` -> nginx (5 s cache) -> `rooms/mod.rs:506` | `PublicRoomList` (same rows + `bridge_id`, `listed_at`) | same | 15 s per visible tab | per web viewer | HTTPS, no gzip, no ETag | per viewer 0.9-2.2 K |
| D3 | `rankings.js:11,673,717`; nginx 15 s; bridge `PublicCache` `ranking.rs:72-123` | `PublicBoard` <= 500 `BoardEntry` (`protocol/src/skill.rs:19,81,172`), ~45 B each | <= 22 KB | 30 s per visible tab | per web viewer | HTTPS, no gzip, no ETag | per viewer <= 0.75 K |
| D4 | `TournamentPlay.hxx:27,34`; `WatchPlay.hxx:25`; `IdentityMatches.cxx:17,20`; `IdentitySignIn.cxx:19`; `IdentityPanel.cxx:19` | `Claim`/`ClaimAnswer`, prepare, `WatchAnswer`, assignments refresh, sign-in poll | 0.5-2 KB each incl. TLS/headers | 10 s, <= 2 s while waiting for a permit, 10 s, 60 s (5 s for 180 s after a lost link), 4 s during sign-in, 30 s retry | per player | HTTPS | per player 0.05-1 K |
| D5 | `service/short_links.rs:9,357`; `short_invite.rs` | sealed invite PUT | ~0.6 KB | on change + 10 min | room leader | HTTPS | ~0.001 K |
| D6 | `service/refresh.rs:12-38` | invite renewal -> `Event::DiscordInvite` | ~1 KB | checked 1 s, fires once per ~16.8 h (10% of 7 days) | room | pipe | ~0 |
| S1 | `bridge/src/routes/rooms/poll.rs:21-31,92-98,155-186` | bridge `GET {supervisor}/rooms` -> `Vec<Reported>` (`rooms/supervisor.rs:79`: invitation ~350 B, `banned` <= 512, `details`, `games`) ~0.6-1.5 KB/room; then one SQLite write txn with an `UPDATE` and `record_games` per room, then `state.committed()` | ~45 KB at 45 rooms | 5 s | bridge <-> supervisor, all rooms | loopback HTTP + SQLite | VPS-wide ~9 KB/s + 1 write txn/5 s |
| S2 | `bridge/src/lib.rs:82-87`, `routes/events.rs:134-159` | `committed()` wakes every SSE stream (auth check + `events::list` read) and the delivery workers | DB reads | every 5 s poll and 10 s maintenance (`routes/mod.rs:39,187-253`), even with no change | every open event stream | SQLite | CPU only |
| S3 | `events.rs:24,159`; `delivery.rs:104-111`; notifier `inbox.rs:102-130`; brackets `bracket.rs:38`, `reconcile.rs:20` | SSE keepalive comment; delivery lease pass; inbox SELECT; bracket reads | small | 20 s; 1 s x 4 queues; 1 s; 60-600 s; 300 s | VPS | SSE / SQLite / HTTP | small |
| S4 | `server/roomhost/RoomHost.cxx:194-237` | `StatusLine` (`RoomHostStatus.cxx:91-103`) | ~1 KB | on change only (revision fence + full compare); tick 16 ms / 250 ms quiet | room host -> supervisor | stdout | ~0 |

Ranking by bytes per second (busy 16-member room): A1 Raft append fan-out (~660-700 KB/s, VPS egress
in a public room) >> A2 pipe export (~306 KB/s per member, local) > A3 live snapshots (~82 KB/s) >
B1 authority reads (~11-12 KB/s) > B4 keepalives with the board open (~6.5 KB/s) > B2 leader
barriers (~4.4 KB/s) > D1/D2 rooms list (~1-2 KB/s per viewer) > C3 > everything else (< 1 KB/s).
At 2 members the order is A2 > A1 (3.7-7.4 KB/s) > A3 ~= B1. A1 and A2 are entirely driven by P,
and P is mostly a journal of superseded projections (F-OVH-1).

Ranking by CPU (per tick or per commit):
1. Leader/host, per commit: `CompressedJsonWire::encode` runs once per follower on an identical entry:
   `serde_json::to_vec` of the request (escaping the 280-460 KB checkpoint string), zlib fast, blake3,
   base64, then `Peer::request` serializes the wire again (`coordination.rs:79-98,1022-1039,1042-1053`);
   `try_get_log_entries` clones the entry per send (`:716-728`). Estimate 3-5 ms per follower: 45-80
   ms per commit at 16, on the two runtime workers that also carry every gameplay bridge (F-OVH-3).
2. Every member, per commit: the helper parses the whole checkpoint into `serde_json::Value` twice
   (`seats.rs:180`, `coordination/latency_roster.rs:74`), hashes it (`Committed::digest`), copies it
   (`checkpoints.rs:298`), base64s it into ~30 JSON events; the game parses ~30 events
   (`IrohRoom.cxx:794-796`, budget 8 per poll) and then, on the worker, parses ~460 KB, dumps the
   effects for their digest (`SessionRecovery.hxx:311-322`) and dumps every journal entry again in
   `CompactEffectJournal` (`:245-250`; `CheckpointDecodeWorker.hxx:52-61`); import on the game thread
   (~1.1 ms per the baseline). (F-OVH-1, F-OVH-2, F-OVH-4.)
3. Every member, per commit, game thread: its own room snapshot is parsed 3-4 times and dumped once
   (F-OVH-5).
4. Raft `build_snapshot` every 3 (host) or 8 commits: whole `Machine` to JSON plus zlib (`coordination.rs:731-745`).
5. 500 Hz pollers: the game's helper pipe thread (`HelperClient.cxx:144-181`, 2 ms waits) and the
   actor's 2 ms `FAST_TICK` while a game or transfer is active (`service/mod.rs:55`, `pump_committed_checkpoint`
   takes the store mutex every tick, `checkpoints.rs:271`). Microseconds each; wakeups, not work.
6. 1 Hz: coordination refresh (B1/B2), latency second (row rebuild + compare). 4 Hz: stall watchdog.

## 3. Findings

### F-OVH-1: The replicated effect journal keeps superseded room projections until its 256 KB budget, so every commit ships ~250 KB of dead snapshots to every follower and every member's game
- File: src/session/SessionRecovery.hxx:197-243
- File: src/session/SessionRecovery.hxx:245-250
- File: src/session/sf4e__SessionServer__Recovery.cxx:348-361
- File: src/session/sf4e__SessionServer__RecoveryCheckpoint.cxx:289-290
- File: src/session/CheckpointDecodeWorker.hxx:55-61
- Category: streaming
- Severity: P1
- Claim: `CompactEffectJournal` removes a `room_snapshot`/`data_update` that a later one for the same
  recipient supersedes only inside `while (history.size() > MaxEffectJournalEntries || bytes > MaxEffectJournalBytes)`
  (`SessionRecovery.hxx:210-217`). Every commit's `BroadcastRoomState` journals a fresh snapshot and
  `data_update` for every member (`sf4e__SessionServer__Rooms.cxx:391-433`), so the committed history
  fills to 256 KB / 256 entries within a few dozen commits in any room, 2 members included, and stays
  there. `RecoveryCheckpoint()` copies that whole history into `effect_journal` (`:290`). Worse, the
  journal in a proposal is the history *before* this commit, while `proposal.effects` already carries
  a newer projection for every member whenever the commit ran `BroadcastRoomState` (nearly every room
  mutation; an acknowledgement-only commit may not), so on arrival nearly every projection in the
  journal is superseded;
  each follower's decode concatenates journal + effects and compacts (`CheckpointDecodeWorker.hxx:57-59`),
  discarding them. Net: P is ~280 KB at 2 members where the live state is ~30 KB; this is most of the
  ~460 KB that `ROOM_PERF_BASELINE.md` says every member still decodes, and it is the 320 KiB the
  Raft config comment budgets for (`coordination.rs:1118-1121`).
- Proposed change: (1) In `CompactEffectJournal(history, encoded)`, always drop a supersedable
  projection that has a later entry with the same `(recipient, type)`, before the budget loop, except
  keep the latest `room_snapshot` per recipient that carries chat when a later one for that recipient
  says `chat_unchanged` (visible in the payload, `sf4e__SessionProtocol.hxx:538`). (2) In
  `SessionServer::RecoveryCheckpoint()`, write `effect_journal` without entries superseded by an
  effect of the same `(recipient, type)` in `_candidate` (the proposal's own effects), under the same
  chat exception. The only one-argument restore of a checkpoint is the leader's local
  `_recoveryBaseline` (`sf4e__SessionServer__Recovery.cxx:94`), built before any candidate effect exists;
  the replicated path always restores journal + effects (`RoomRecoveryRuntime.hxx:76`).
- Behavior preserved: every lifecycle, result, `game_*` and private record is kept exactly as today;
  the newest projection per member (and the newest chat-bearing one) is kept, so a successor's replay
  and a member's own fallback delivery (`IrohRoomCheckpoint.cxx:261-265`) end at the same state. The
  budget pass already deletes superseded projections first, so the format and every reader are
  unchanged. No wire or JSON shape change: an old follower (a client behind a newer public room host)
  accepts a shorter journal, as it already does after a budget pass. This is not the per-table room
  delta the baseline rejected: the live 13 KB snapshot protocol is untouched.
- Behavior at risk: a member whose live control send of an older projection was lost and whose
  later projection is also lost would replay only the newest instead of older-then-newest (it ends
  at the same state). Determinism of the rollback sim is untouched (room plane only). Run
  `IrohRecoveryIntegrationTest` and the chat-delta tests.
- Estimated saving: P from ~280 / 320 / 460 KB to ~35 / 120 / 280 KB (2 / 8 / 16 members). Raft
  egress per busy room: 2 members 3.7 -> ~0.5 KB/s (private), 8 members 107 -> ~40 KB/s, 16 members
  660 -> ~400 KB/s; public-room VPS egress similarly (704 -> ~430 KB/s at 16). Pipe export and the
  decode/compaction CPU on every member fall in the same ratio (about 8x at 2 members, 1.6x at 16).
  The host's per-follower compression (F-OVH-3) falls with it. At 16 members the residual mass is
  `proposal.effects` itself (capped at 256 KB by the candidate budget): N projections, each needed by
  only one member. That is what a per-recipient scheme would address; it is distinct from the
  per-table live delta the baseline rejected, and is not proposed here.
- Confidence: medium (mechanism high; sizes depend on room activity and the compression ratio)

### F-OVH-2: Every member's helper parses the full committed checkpoint into `serde_json::Value` twice per commit, plus once per probe, to read a few fields
- File: rust/sf4-net/src/service/seats.rs:180
- File: rust/sf4-net/src/coordination/latency_roster.rs:73-74
- File: rust/sf4-net/src/coordination.rs:633-640
- File: rust/sf4-net/src/service/latency/authority.rs:204-215
- Category: overhead
- Severity: P2
- Claim: `observe_commit` (`seats.rs:169-196`, cached per commit) and the latency authority's
  `roster` (`authority.rs:204-215`, cached per commit) each build a full `serde_json::Value` tree of the
  280-460 KB checkpoint, most of which is `effect_journal` and `effects` they never read;
  `committed_probe_pair_bound` builds a third tree for every probe. The latency cache key also forces
  `Committed::digest()` (SHA-256 of the whole checkpoint). Each tree is tens of thousands of small
  allocations on the helper's two runtime workers, which also run every gameplay bridge.
- Proposed change: add `Committed::view()` (a `OnceLock<Result<CommittedView, _>>` beside `digest`)
  that deserializes once into a typed struct with only `checkpoint.members`, `checkpoint.room.snapshot.{members,tables}`
  and the tournament seats, skipping `effect_journal`, `effects` and the rest with
  `serde::de::IgnoredAny`. `observe_commit`, `committed_latency_roster` and
  `committed_probe_pair_bound` read the view; key the latency cache on `(revision, term)` plus
  `Arc::ptr_eq` like `same_commit` instead of the digest.
- Behavior preserved: the same fields with the same validation; malformed input still yields
  `None`/`RosterInvalid`. Helper-internal; the pipe and the wire are unchanged.
- Behavior at risk: none identified (same-install code; the view must accept both
  `authenticatedEndpoint` spellings as today).
- Estimated saving: two to three full JSON tree builds of ~460 KB per commit per member (estimated
  5-10 ms of helper CPU at 16 members, ~1 ms at 2 after F-OVH-1), plus one SHA-256 of the checkpoint.
- Confidence: high

### F-OVH-3: The leader serializes and zlib-compresses the same Raft entry once per follower
- File: rust/sf4-net/src/coordination.rs:1041-1053
- File: rust/sf4-net/src/coordination.rs:79-98
- File: rust/sf4-net/src/coordination.rs:1022-1039
- File: rust/sf4-net/src/coordination.rs:716-728
- Category: overhead
- Severity: P2
- Claim: `Peer::append_entries` runs `CompressedJsonWire::encode(&request)` per follower: JSON of the
  request (re-escaping the whole checkpoint string), zlib, blake3, base64, then `Peer::request` runs
  `serde_json::to_vec` of the wrapper again. For in-sync followers OpenRaft builds identical requests
  (same `vote`, `prev_log_id` = matching, same entries, usually the same `leader_commit`). At 16
  members that is 15 (private) or 16 (public host on the VPS) identical encodes of ~460 KB per commit,
  estimated 45-80 ms of CPU per commit on the leader's runtime; on a private-room leader that runtime
  is shared with the leader's own gameplay bridges (`ROOM_PERF_BASELINE.md`, "the 2 ms tick shares the
  runtime's two workers with every gameplay bridge").
- Proposed change: give `Network` a one-slot memo `Mutex<Option<(AppendKey, Arc<Vec<u8>>)>>` where
  `AppendKey = (vote, prev_log_id, leader_commit, first and last entry LogId)`; `append_entries` looks
  it up before encoding and stores the final RPC body bytes after. Optional second step: keep
  `Proposal.checkpoint` as `Box<RawValue>` so it is not escaped; that changes the log entry's JSON
  shape and needs a capability fence (see "Behavior at risk").
- Behavior preserved: byte-identical RPC bodies; followers see no difference. No wire change for the
  memo.
- Behavior at risk: none for the memo. The RawValue step changes the append body; private rooms
  share one DLL hash (`sf4e__SessionServer.cxx:504`, `JR_HASH_INVALID`, a netplay invariant) and the
  helper ships with the DLL, but a public room host's helper can be newer than its learners, so it
  would need a capability in `Admission` and a v1 fallback.
- Estimated saving: (F-1)/F of append encode CPU, about 94% at 16 members (~40-75 ms per commit on
  the leader). Bytes unchanged.
- Confidence: medium (hit rate depends on followers being in step; high for public rooms where the
  host commits alone)

### F-OVH-4: The committed checkpoint crosses the pipe as ~30 base64-in-JSON events parsed on the game thread
- File: rust/sf4-net/src/service/checkpoints.rs:464-532
- File: rust/sf4-net/src/recovery.rs:24-25
- File: src/session/IrohRoom.cxx:792-796
- File: src/session/IrohRoomCheckpoint.cxx:309-325
- File: rust/sf4-net/src/wire.rs:13-16
- Category: overhead
- Severity: P2
- Claim: each commit is exported as `CheckpointChunk` events of 16 KiB raw (`CHECKPOINT_CHUNK_BYTES`),
  base64 inside a JSON object, window 4, each acknowledged by a JSON `checkpoint_ack`. The game parses
  every event with nlohmann on the game thread, eight per `Poll` (`IrohRoom.cxx:794`), then base64
  decodes. At 460 KB that is ~29 chunk events + ~29 acks + 4/3 bytes. The leader's own proposal goes
  the other way the same way (`PumpCheckpoint`, `IrohRoomCheckpoint.cxx:309-325`). The IPC frame
  allows 512 KiB (`MAX_IPC_PAYLOAD`).
- Proposed change: preferred: a binary IPC frame kind for chunk data (the frame layer is already
  length-prefixed binary, `wire.rs:41-60`), so a chunk is a memcpy and only a small JSON identity is
  parsed. Fallback: raise `CHECKPOINT_CHUNK_BYTES` (Rust) and `coordination::CheckpointChunk` (C++) to
  128 KiB with a window of 2. That cuts event and ack counts but moves the same parse work into fewer,
  larger game-thread spikes (`Poll` takes up to 8 events per call), so it relocates the cost rather
  than removing it.
- Behavior preserved: same transfer identity, ordering, credit and digest checks; helper and game ship
  together, so no cross-version case.
- Behavior at risk: larger single events delay other events in the pipe by one bigger frame; keep the
  lifecycle reserve (`LIFECYCLE_EVENT_RESERVE`) logic as is.
- Estimated saving: per member per commit ~22 fewer JSON parses on the game thread and ~22 fewer acks
  (chunking change) or all of the base64 and ~25% of pipe bytes (binary frame): 100-150 KB/commit of
  pipe traffic at 16 members, estimated 2-4 ms of game-thread parse per commit spread over frames.
- Confidence: medium

### F-OVH-5: A member's room snapshot is parsed three to four times and re-dumped once on the game thread per commit
- File: src/session/IrohRoomPeers.cxx:283-288
- File: src/session/IrohRoom.cxx:545-611
- File: src/session/IrohRoomAdapters.cxx:105-123
- File: src/session/sf4e__SessionClient.cxx:534
- File: src/session/IrohRoomCheckpoint.cxx:245-260
- Category: overhead
- Severity: P3
- Claim: an inbound control message (`Event::Message.payload`, a JSON-escaped String) is copied out of
  the event and parsed in `ReceiveControlMessage`; `AuthorizedEffect` parses it again and dumps it back
  after stripping `_commit`; `ClientAdapter::Receive` parses it a third time to read `type`;
  `SessionClient` parses it a fourth time. A 16-member snapshot is ~13 KB, ~39 KB with chat. Separately,
  `ActivateCommittedCheckpoint` parses every queued client message on every commit just to look for
  `_terminal_replay` (`IrohRoomCheckpoint.cxx:245-260`).
- Proposed change: keep the parsed `json` (or at least `type` and a `terminalReplay` key) on
  `IrohRoom::Message` from the first parse; `AuthorizedEffect` erases `_commit` in place and hands the
  object on; `ClientAdapter` reads the stored type; the activation pass checks the stored key. The
  Rust side can emit `Event::Message.payload` as `RawValue` like `NativeControlMessage` does
  (`protocol.rs:526-552`) so it is not escaped.
- Behavior preserved: same validation and ordering; pipe-only shape change (same install).
- Behavior at risk: none identified; SessionClient still receives a string unless its interface is
  widened too.
- Estimated saving: two to three ~13-39 KB parses and one dump per member per commit on the game
  thread (sub-millisecond to ~1 ms at 16 members). Same pattern, same fix (track the size instead of
  dumping to measure it): `SessionServer::Checkpoint()` and `RestoreCheckpoint` each run a full
  `value.dump()` only to compare with `MaximumCheckpointBytes`
  (`sf4e__SessionServer__RecoveryCheckpoint.cxx:59,66`), on the leader per commit and on every member
  per import, and `RestoreRecoveryCheckpoint(const json&)` dumps the journal for the same reason (`:296`).
- Confidence: medium

### F-OVH-6: The rooms list and the rankings board are refetched whole every 15 s / 30 s with no compression and no revalidation
- File: server/ember-short/deploy/site/assets/rooms.js:9
- File: server/ember-short/deploy/site/assets/rooms.js:243
- File: server/ember-short/deploy/site/assets/rankings.js:11
- File: server/ember-short/deploy/nginx/ember-short-locations.conf:83-88
- File: server/ember/bridge/src/routes/rooms/mod.rs:423-466
- File: src/ui/PublicRoomsPanel.hxx:40
- File: rust/sf4-net/src/tournament/rooms.rs:121-130
- Category: overhead
- Severity: P2
- Claim: the answer to "the large JSON every 15 s" (section 0). `/rooms.json` and `/rankings.json`
  are plain JSON (`gzip_types` covers only css/js/text in `/assets/`), have no ETag, and the pages
  fetch with `cache: 'no-store'`, so each poll moves the full 13-33 KB (rooms) or <= 22 KB (board).
  The in-game panel does the same against `GET /v1/rooms?detail=1`, which the bridge recomputes per
  request (one SQLite read, `serde_json::to_value` then serialized again, `mod.rs:438-465`) and the
  game re-parses even when nothing changed. The public board is cached as a struct and re-serialized
  for every request (`ranking.rs:101,122`).
- Proposed change: (1) nginx: `gzip on; gzip_types application/json;` on the `/rooms.json` and
  `/rankings.json` locations and on the bridge vhost's `/v1/rooms`. (2) Bridge: send
  `ETag: "<blake3 of body>"` on `public_list`, `list` and `public_board`, answer `If-None-Match` with 304;
  cache the serialized bytes of the public board instead of the struct. (3) Site: fetch with
  `cache: 'no-cache'` so the browser revalidates. (4) Helper: remember the last ETag per bridge and
  send `If-None-Match`; on 304 answer the game with an `unchanged` flag so `PublicRoomsPanel` keeps
  its list and only updates `listed`.
- Behavior preserved: the same list, the same refresh cadence, the same "updated N s ago" text.
  Old clients send no `If-None-Match` and get 200 as today; an old bridge ignores the header and
  answers 200, which the new helper handles as today.
- Behavior at risk: none identified; `listed_at` is inside the body, so an unchanged body must keep
  the same `listed_at` (or the ETag must exclude it) for 304s to happen. Decide which.
- Estimated saving: gzip cuts each answer ~75-85% (~13 KB -> ~2.5 KB at 45 rooms); 304s cut unchanged
  polls to ~200 bytes and skip the bridge's re-serialization and the game's parse. Per viewer
  ~0.9-2.2 KB/s -> ~0.05-0.2 KB/s.
- Confidence: high (sizes medium)

### F-OVH-7: Every follower asks the leader for a fresh authority claim every second, and the leader runs a quorum barrier for it
- File: rust/sf4-net/src/service/mod.rs:1505-1508
- File: rust/sf4-net/src/recovery.rs:407-452
- File: rust/sf4-net/src/coordination.rs:1186-1273
- File: rust/sf4-net/src/coordination_iroh.rs:732-741
- Category: overhead
- Severity: P3
- Claim: `emit_coordination_state` runs on the 1 s statistics arm and calls `RecoverySession::state()`,
  which on a follower is an `authority` RPC to the leader (new QUIC bi-stream, ~0.7 KB round trip) and
  on the leader an `authority_claim` whose 500 ms coalesce is shorter than the 1 s period, so its own
  check misses the cache unless a follower's claim refilled it within the last 500 ms and runs `ensure_linearizable` (empty appends to every other voter).
  Followers' claims arrive at random phases and trigger up to one more barrier per second. Result is
  emitted to the game only on change (`refresh.rs:313-335`). Fan-out is N-1 per room (N in a public
  room, all into the VPS host): ~11-12 KB/s at 16 members, continuously while a room is open.
- Proposed change: keep the cadence (it bounds how fast `writable` drops and the room pauses,
  `sf4e__SessionServer__Rooms.cxx:206-219`), but (a) set `AUTHORITY_CLAIM_COALESCE` a margin above the 1 s
  statistics period (e.g. 1.2 s; exactly 1 s races timer jitter) so the leader's own check and its
  followers' claims share about one barrier per second; (b) for
  `Ownership::Member` learners of a server-owned room, where the sole voter's barrier is local and
  proves nothing a follower could contradict (`coordination.rs:1239-1246`), answer the learner from
  the host's cached claim (already 2.5 s) and consider letting the learner skip the RPC when an
  authenticated append from the host's current term arrived within the last second.
- Behavior preserved: same `writable` semantics and period for private rooms; (a) only changes how
  many barriers run, not when a claim is fresh enough (a 1 s-old claim at a 1 s period).
- Behavior at risk: (a) a claim served up to 1.2 s old instead of 0.5 s; (b) changes quorum-loss
  detection inputs for public-room members. Both touch the room-pause timing, not the rollback sim.
- Estimated saving: (a) ~half the leader's barrier appends (~2 KB/s in 5-voter rooms); (b) up to
  ~12 KB/s of VPS ingress+egress per full public room. Small; listed because it is the one
  per-second item with per-member fan-out.
- Confidence: medium

### F-OVH-8: The bridge rewrites every public room and wakes every event stream every 5 s even when nothing changed
- File: server/ember/bridge/src/routes/rooms/poll.rs:85-98
- File: server/ember/bridge/src/routes/rooms/poll.rs:150-186
- File: server/ember/bridge/src/lib.rs:82-87
- File: server/ember/bridge/src/routes/events.rs:134-159
- Category: overhead
- Severity: P3
- Claim: `poll` fetches the supervisor's full `Vec<Reported>` (~45 KB at 45 rooms; the supervisor
  serves it from memory, `ember-rooms/src/supervisor.rs:369`), runs one write transaction with an
  `UPDATE rooms` and `skill::record_games` per room, and calls `state.committed()` whenever the
  transaction succeeds. `committed()` wakes every SSE stream (an auth check and an `events::list`
  read each) and the four delivery workers. Room hosts already report only on change
  (`server/roomhost/RoomHost.cxx:208-232`), so most polls carry identical rows.
- Proposed change: keep a per-room digest of the last applied `Reported` in memory; skip the UPDATE,
  bans and `record_games` for an unchanged room; have `apply` return whether it wrote an event and
  call `committed()` only then. Optionally have the supervisor expose a generation counter
  (`GET /rooms?since=g` -> 304) with a full answer when the parameter is unknown.
- Behavior preserved: same database contents and events; same 5 s detection of closed rooms (the
  absence check still runs every poll).
- Behavior at risk: `opened` latching and `record_games` de-duplication must still see every
  transition; a digest per room keeps that.
- Estimated saving: ~9 SQLite row writes/s at 45 rooms down to rows that changed; one fewer wake of
  every event stream per 5 s.
- Confidence: medium

### F-OVH-9: The game's helper pipe thread and the actor tick poll at 500 Hz
- File: src/platform/HelperClient.cxx:144-181
- File: rust/sf4-net/src/service/mod.rs:55
- File: rust/sf4-net/src/service/checkpoints.rs:265-275
- Category: overhead
- Severity: P3
- Claim: `HelperClient::Run` loops `PeekNamedPipe` + `WaitForSingleObject(stop_, 2)` for the whole
  session, so an idle game wakes 500 times a second and an outgoing command waits up to 2 ms. The
  helper actor's `FAST_TICK` (2 ms) runs `pump_committed_checkpoint`, which locks the store and clones
  the `Arc<Committed>` every tick, although `Store::committed_changes` exists for this.
- Proposed change: overlapped `ReadFile` on the pipe plus an event signalled by `outgoing_.Push`,
  waited with `WaitForMultipleObjects` (the startup read already does this, `HelperClient.cxx:69-75`);
  in the actor, add a `committed_changes()` arm to the `select!` and drop the per-tick store read.
- Behavior preserved: same ordering and backpressure (the pending-incoming slot stays).
- Behavior at risk: pipe latency becomes lower, not higher; none identified for determinism.
- Estimated saving: ~500 wakeups/s on a game thread pool and up to 2 ms of command latency; CPU
  small.
- Confidence: medium

### F-OVH-10: The membership snapshot is serialized once per peer on each publication
- File: rust/sf4-net/src/service/controls.rs:308-336
- File: rust/sf4-net/src/service/controls.rs:345-370
- Category: overhead
- Severity: P3
- Claim: `pump_membership_publications` calls `send_membership_control` per peer, and each call
  rebuilds the filtered `Admission` list and runs `serde_json::to_string` of the whole
  `CoordinationControl::Membership` (up to 16 `Admission`s of ~400 B each plus up to 128 retired ids,
  ~7-9 KB). Publications are event-driven (a new leader or a retirement, `refresh.rs:236-241`,
  `members.rs:581`), so this is not periodic.
- Proposed change: build the payload once per pump when `retired_before_retry` is empty (the pump's
  case) and send `Arc<[u8]>`/cloned bytes to each peer.
- Behavior preserved: identical frames.
- Behavior at risk: none identified.
- Estimated saving: N-2 serializations of ~8 KB per publication; small.
- Confidence: high

## Area summary

- Health: the periodic plane is already lean. Raft heartbeats are empty (9 s), room hosts report on
  change, latency and coordination events are send-on-change, and a quiet room commits nothing.
  Nothing heavy is sent on a timer except the 15 s rooms list (section 0, F-OVH-6).
- The cost is per commit, and it is dominated by one thing: the checkpoint's effect journal, which
  sits at its 256 KB budget full of projections that the same proposal already supersedes
  (F-OVH-1). It multiplies through Raft fan-out (VPS egress in public rooms), the pipe export, and
  every member's decode. Pruning it is the biggest judo move: no protocol change, old readers
  unaffected, P shrinks ~8x in small rooms.
- Biggest overhead item: A1, Raft append of the full proposal to every follower, ~660-700 KB/s in a
  busy 16-member room (estimate; 8x compression and 0.5 commits/s assumed), then the matching pipe
  export (A2).
- On the per-table delta question in ROOM_PERF_BASELINE.md: nothing here proposes changing the 13 KB
  live snapshot; F-OVH-1/3/4 attack the 460 KB the baseline left in place, without a protocol change.
- Could not assess: actual compression ratio and commit rates (no run allowed; RoomHostBench should
  print compressed append bytes); whether a public-room learner's native import of every commit
  could be skipped entirely (activation and effect authorization depend on it,
  `RoomRecoveryRuntime.hxx:88-105`); the VPS's http-level nginx gzip settings (not in the repo).
