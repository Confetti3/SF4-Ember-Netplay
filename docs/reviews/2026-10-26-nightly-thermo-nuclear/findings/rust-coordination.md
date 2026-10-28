# Rust coordination, recovery, transport, invites, probe: findings

Scope: rust/sf4-net/src/coordination.rs, coordination/{latency_roster,server_owned}.rs,
coordination_iroh.rs, coordination_iroh/retired.rs, recovery.rs, transport.rs,
transport/{fixed_port,with_proof}.rs, invite.rs, short_invite.rs, probe.rs (test modules skipped).
Read-only review at nightly b9ce0e31.

## Compatibility preamble (referenced by every wire finding below)

- Helper to helper (Raft "append", "snapshot", "vote", "authority", "propose", "elect", "remove" on ALPN
  `sf4e/coordination/1`): every member of a room was admitted by `Invite::admit_member`, which refuses a
  proof whose `build` differs (invite.rs:493; joiner side invite.rs:137; PUBLIC_ROOMS.md:328 for public
  hosts). `build` is the native sidecar hash (sf4e__SessionClient.cxx:196), a per-release value. So no
  coordination group ever mixes builds, and a coordination wire change ships atomically with the build.
  Still bump the ALPN to `sf4e/coordination/2` with any change so a stray cross-build dial fails at the
  QUIC handshake instead of at a JSON parse.
- Helper to native (IPC `checkpoint_begin/chunk/end/ack/committed`): sf4-net.exe ships in the same package
  as the game DLL, so the IPC can change in one release.
- The only truly cross-version surfaces here are the invitation texts (`sf4e1:`, `sf4e2:`, `sf4e3:`,
  `emd1:`, `emd2:`) and the short-link service wire (`server/ember-short`). Nothing below changes them.

## Where the Raft checkpoint bytes go today (one commit, room of N members)

Leader helper, per follower (N-1 times, all learners included; a public host has up to 16 learners):
`try_get_log_entries` clones the `Entry` (checkpoint `String`, ~320-460 KB) -> `serde_json::to_vec` of the
whole `AppendEntriesRequest` (the checkpoint is JSON text, so every `"` is escaped again, roughly +10%)
-> zlib (miniz_oxide, `Compression::fast`) -> blake3 over the raw bytes -> base64 (+33%) -> wrapped in
`CompressedJsonWire` and `serde_json::to_vec` again in `Peer::request` -> `write_body`.
Follower: JSON parse wrapper (copies the base64 string) -> base64 decode -> inflate -> blake3 -> JSON
parse of the request (unescapes the checkpoint into a new `String`) -> apply clones it into `Committed`
-> `pump_committed_checkpoint` copies it to a `Vec` -> SHA-256 -> base64 16 KB chunks over the IPC pipe
-> native base64-decodes, SHA-256s and parses with nlohmann (the 11 ms decode in ROOM_PERF_BASELINE).
The leader's native process gets its own proposal echoed back the same way (F-COORD-6).

---

### F-COORD-1: The leader compresses and re-encodes every append once per follower
- File: rust/sf4-net/src/coordination.rs:1042
- File: rust/sf4-net/src/coordination.rs:81
- File: rust/sf4-net/src/coordination.rs:716
- Category: overhead
- Severity: P1
- Claim: `Peer::append_entries` (1042-1053) runs `CompressedJsonWire::encode(&request)` for each RPC, and
  OpenRaft creates one `Peer` and one replication stream per target. Every committed checkpoint is therefore
  JSON-serialized (with re-escaping of the embedded JSON text), zlib-compressed, blake3-hashed, base64-encoded
  and JSON-wrapped N-1 times on the leader, each time from a fresh clone of the entry
  (`try_get_log_entries`, 720-727, `e.clone()`). The work is identical for every follower: the entry is
  immutable once it has a `LogId`. In a 16-member private room that is 15 encodes per Ready, chat line, ack or
  result; on a public host it is 16 per commit, times up to 45 rooms on one VPS. All of it runs inside
  `async fn`s on the helper's tokio workers, which ROOM_PERF_BASELINE.md says the 2 ms tick shares with every
  gameplay bridge. RoomHostBench is network-free, so this cost was never in the baseline's numbers.
- Proposed change: compress the checkpoint once, at the source, and carry the bytes. Concretely: in
  `RecoverySession::propose` build `Proposal { checkpoint: CompressedCheckpoint }` where
  `CompressedCheckpoint` holds `Arc<[u8]>` zlib bytes plus raw length and is produced once on the leader;
  `Peer::append_entries` sends a binary envelope (see F-COORD-2) whose entry bodies are those bytes as-is;
  followers inflate once in `apply_to_state_machine` (or lazily on first `Committed::checkpoint()`).
  `encode_snapshot` then also reuses the compressed bytes instead of re-compressing the checkpoint.
  A smaller first step with no wire change: memoize the encoded `CompressedJsonWire` bytes in `Network`
  (shared by all `Peer`s) keyed by `(vote, prev_log_id, leader_commit, entry LogIds)`; the first send of a
  new entry to all followers usually shares that key, so most of the N-1 encodes become one. The memo is
  fragile (any `leader_commit` difference misses), so treat it as a stopgap.
- Behavior preserved: Raft semantics, entry content, commit order, apply validation, receipts, snapshot
  content, all timeouts.
- Behavior at risk: wire compat for the full change (build-gated, see preamble; bump ALPN). The memo-only
  step changes nothing on the wire. Apply must keep the `MAX_CHECKPOINT` bound by checking the declared raw
  length before inflating (as `CompressedJsonWire::decode` does today).
- Estimated saving: per commit on the leader, (N-1) x [escape ~0.5 ms + zlib-fast of ~350-500 KB ~3-5 ms +
  blake3 + base64 + 2 copies] collapses to one compress. N=16: roughly 50-80 ms of worker CPU per commit
  down to ~4 ms (estimate, not measured; miniz_oxide fast level on JSON). Removes 15 transient
  ~0.5-1 MB allocations per commit. Followers lose one JSON unescape pass.
- Confidence: high on the structure (code is unambiguous), medium on the millisecond figures.

### F-COORD-2: base64 and JSON wrap binary bodies on a length-framed binary stream
- File: rust/sf4-net/src/coordination.rs:73
- File: rust/sf4-net/src/coordination.rs:128
- File: rust/sf4-net/src/coordination.rs:1021
- File: rust/sf4-net/src/coordination_iroh.rs:691
- Category: overhead
- Severity: P2
- Claim: the coordination stream already frames every body with a u32 length (`write_body`/`read_body`,
  coordination_iroh.rs:659-703). On top of that, the append body is zlib bytes base64-encoded into
  `CompressedJsonWire.data: String` and the whole struct JSON-serialized (`Peer::request`, 1031), with
  `digest: [u8; 32]` written as a JSON array of 32 decimals. Install-snapshot chunks do the same
  (`InstallSnapshotWire.data`, 64 KB chunks become ~87 KB). The checkpoint itself is JSON text carried as a
  JSON string inside JSON (escaped before compression). The blake3 digest duplicates zlib's own adler32 and
  QUIC's AEAD; the sender is already authenticated by endpoint key, so it adds no protection a malicious
  member could not recompute. Even empty heartbeats pay zlib + blake3 + base64 (F-COORD-11).
- Proposed change: one binary codec module (`coordination/wire.rs`): append = fixed header
  (vote, prev_log_id, leader_commit as little-endian ints or a tiny serde_json header) followed by each
  entry's compressed bytes with a length; snapshot = header + raw chunk bytes. Drop the base64 and the
  outer JSON for both. Keep the declared-length check and the `take(MAX_SNAPSHOT + 1)` inflate bound;
  drop the blake3 digest (or keep it only if a reviewer wants a parse-before-trust check). Small RPCs
  (vote, authority, propose, elect, remove) can stay JSON.
- Behavior preserved: the decoded `AppendEntriesRequest`/`InstallSnapshotRequest` values, every size bound,
  every validation in `dispatch`.
- Behavior at risk: wire compat only (build-gated; bump ALPN to `sf4e/coordination/2`). No old client can be
  in the group.
- Estimated saving: 25% of every append and snapshot body on the wire (base64 removal) plus ~10% of the
  pre-compression size (no double escape). Example: a 90 KB compressed checkpoint goes out as ~120 KB today;
  saving ~30 KB per follower per commit, ~450 KB of leader uplink per commit at N=16. CPU: one fewer base64
  pass and one fewer JSON copy per hop per side.
- Confidence: high

### F-COORD-3: The checkpoint `String` is copied four to six times per commit per node
- File: rust/sf4-net/src/coordination.rs:175
- File: rust/sf4-net/src/coordination.rs:882
- File: rust/sf4-net/src/coordination.rs:985
- File: rust/sf4-net/src/recovery.rs:593
- File: rust/sf4-net/src/service/checkpoints.rs:293
- Category: overhead
- Severity: P2
- Claim: copies of the full checkpoint (~320-460 KB): `recovery::propose` does
  `String::from_utf8(transfer.bytes.clone())`, and the original `transfer` is then kept alive in the
  proposal task only to compare and re-send the same bytes (`completed_checkpoint_proposal`); each OpenRaft log read clones the `Entry` (720-727, once per follower per send);
  `apply_to_state_machine` clones `proposal.checkpoint` into `Committed` (884); `build_snapshot` clones the
  encoded Vec (740) and `get_current_snapshot` clones it again per lagging learner (985);
  `pump_committed_checkpoint` copies it into a `Vec` for the IPC transfer. `Cargo.toml:31` already enables
  serde's `rc` feature, so `Arc<str>` serializes and deserializes exactly like `String`.
- Proposed change: `Proposal.checkpoint: Arc<str>` (or `Arc<[u8]>` once F-COORD-1 lands);
  `Committed.checkpoint: Arc<str>` shares the entry's allocation; `CheckpointTransfer.bytes: Arc<[u8]>` or
  `bytes::Bytes` so the commit path and the IPC sender share one buffer; `Memory.snapshot` holds `Bytes`.
  `recovery::propose` builds the `Proposal` from the transfer's shared buffer instead of a copy.
- Behavior preserved: identical wire bytes (serde `rc` is transparent), identical values everywhere.
- Behavior at risk: none identified; `Committed::same_commit` keeps its pointer test semantics.
- Estimated saving: N-1 + 3 copies of ~400 KB per commit on the leader (about 7 MB of memcpy and 18
  allocations at N=16), 2-3 copies per follower; steady memory drops because the log, the committed value
  and the snapshot no longer each own a copy.
- Confidence: high

### F-COORD-4: `build_snapshot` holds the store mutex while encoding the whole state machine
- File: rust/sf4-net/src/coordination.rs:731
- File: rust/sf4-net/src/coordination.rs:423
- Category: overhead
- Severity: P2
- Claim: `build_snapshot` takes `self.0.lock().await` and keeps it across `encode_snapshot(&memory.machine)`,
  which JSON-serializes the `Machine` (the checkpoint again escaped) and zlib-compresses it, ~3-5 ms for a
  400 KB checkpoint. The same tokio `Mutex` serves `Store::committed()`, which `pump_committed_checkpoint`
  awaits on every actor tick, plus `applied_voters`, `authority_claim`, `probe_*` and Raft's own
  `apply_to_state_machine`. Snapshots are built every 3 commits on a public host and every 8 in a private
  room. The actor tick stalls behind the encode.
- Proposed change: under the lock, `memory.machine.clone()` (cheap: `committed` is an `Arc`; `recent` is
  at most 128 short entries) and release; encode outside; re-lock only to store `(meta, bytes)`. With
  F-COORD-1 the encode no longer recompresses the checkpoint at all.
- Behavior preserved: snapshot content (a consistent copy of the same `Machine` at the same `applied`).
- Behavior at risk: none identified; OpenRaft serializes snapshot builds itself.
- Estimated saving: removes a ~3-5 ms lock hold on the actor's per-tick path every 3 or 8 commits.
- Confidence: high

### F-COORD-5: Two hand-written `serde_json::Value` walks parse the whole checkpoint for a handful of fields
- File: rust/sf4-net/src/coordination.rs:621
- File: rust/sf4-net/src/coordination/latency_roster.rs:73
- File: rust/sf4-net/src/service/latency/authority.rs:213
- Category: overhead
- Severity: P2
- Claim: `committed_latency_roster` (once per commit per member, cached by `Observer::roster`) and
  `committed_probe_pair_bound` (once per probe check, three call sites in service/probes.rs) each build a full
  `serde_json::Value` DOM of the committed checkpoint, including the room chat and the effect journal that
  dominate its size, to read `checkpoint.members[].{member,incarnation,frozen,data.{incarnation,
  authenticatedEndpoint|authenticated_endpoint,latency}}` and `checkpoint.room.snapshot.{members[].{id,
  status,table},tables[].{id,phase,revision,p1,p2}}`. Both duplicate the same alias and fallback navigation
  (incarnation on the row or in `data`; two spellings of the endpoint key), so the rules can drift.
- Proposed change: one typed borrowed view, `coordination/committed_view.rs`:
  `#[derive(Deserialize)] struct CommittedView<'a> { checkpoint: Root<'a> }` with only the fields above,
  `#[serde(alias = "authenticated_endpoint")]`, `Option` for the fallback incarnation, and unknown fields
  ignored (serde skips them without allocating). `committed_latency_roster` and
  `committed_probe_pair_bound` become small functions over `CommittedView`. Optionally parse it once per
  commit next to `Committed` (a `OnceLock<Result<Arc<CommittedView>, _>>` on `Committed`) so probes reuse
  the commit's parse.
- Behavior preserved: the same accepted and rejected inputs (keep the existing tests; add one per alias).
- Behavior at risk: a field that is present with a wrong JSON type: `Value` code treats it as absent, a typed
  struct fails the whole parse. Model such leaves with a small local `deserialize_with` that yields `None` on a
  type mismatch (no new crate), or keep `Value` only for those leaves.
- Estimated saving: per commit per member, a DOM of ~400 KB (thousands of small allocations, every chat line
  and journal string) becomes a skip-scan; estimate 3-6 ms down to under 1 ms (not measured). Probes stop
  paying a full parse each.
- Confidence: medium (saving), high (duplication)

### F-COORD-6: The leader's helper echoes the leader's own committed checkpoint back to native
- File: rust/sf4-net/src/service/checkpoints.rs:331
- File: rust/sf4-net/src/service/checkpoints.rs:585
- File: rust/sf4-net/src/recovery.rs:741
- File: src/session/IrohRoomCheckpoint.cxx:125
- Category: overhead
- Severity: P2
- Claim: native sends its proposal to the helper (base64 chunks over IPC), keeps `proposalBytes_`, and the
  helper proposes it. When it commits, `pump_committed_checkpoint` and `completed_checkpoint_proposal` call
  `start_outgoing_checkpoint(transfer)` unconditionally, so the same bytes go back to the same native
  process: `checkpoint_begin`, 16 KB base64 `checkpoint_chunk` events with acks, `checkpoint_end`; native
  SHA-256s them, base64-decodes and decodes them on the worker, and only then clears `proposalBytes_`
  (IrohRoomCheckpoint.cxx:125-126, matched by transfer and term). The leader is the busiest node.
- Proposed change: when the committed record is exactly the pending proposal
  (`CheckpointProposalKey` already matches it in both places), emit a single
  `checkpoint_committed_local { identity }` event instead of the transfer; native submits its retained
  `proposalBytes_` to `CheckpointDecodeWorker` under that identity (or keeps the decoded form it already
  built) and continues through the same `StageCommittedCheckpoint` / activation path.
- Behavior preserved: ordering (the marker takes the transfer's place in the same queue and is fenced by
  the same `pending_checkpoint_committed` rule), staging, activation, effects.
- Behavior at risk: helper/native IPC shape (same package, ships atomically). Native must refuse the
  marker if `proposalBytes_` was already cleared or the identity differs, and then the helper falls back to
  the full transfer.
- Estimated saving: per commit on the leader, ~400 KB x 1.33 of pipe traffic (~530 KB), 25+ IPC events and
  their JSON framing, one base64 encode and decode, one SHA-256 on each side. Native decode CPU is unchanged
  if it re-decodes `proposalBytes_`.
- Confidence: medium (native side read only around the staging path)

### F-COORD-7: Store -> Coordinator -> RecoverySession is a three-layer pass-through, plus telescoping constructors
- File: rust/sf4-net/src/coordination.rs:1285
- File: rust/sf4-net/src/coordination.rs:1355
- File: rust/sf4-net/src/coordination.rs:563
- File: rust/sf4-net/src/recovery.rs:454
- File: rust/sf4-net/src/recovery.rs:79
- File: rust/sf4-net/src/coordination_iroh.rs:215
- Category: judo
- Severity: P2
- Claim: `Coordinator` forwards ten `Store` reads verbatim (`committed`, `committed_changes`,
  `committed_observed`, `observed_changes`, `applied_voter_ids`, `applied_joint_goal`, `applied_member_ids`,
  `applied_membership_provenance`, `probe_reserved`, `probe_pair_bound`), and `RecoverySession` forwards
  nine of those again, while callers also reach through `recovery.coordinator.*` directly (service code
  calls `coordinator.propose`, `current_term`, `current_leader`), so the wrappers enforce no boundary.
  `probe_reserved` takes seven scalars at each layer (`#[allow(clippy::too_many_arguments)]` twice) to do a
  field-by-field equality against `ProbeReservation`, which already derives `PartialEq`; the Coordinator
  layer even renames them (`peer`, `incarnation` for source and target). Constructors telescope:
  `Coordinator::new/new_for_room/new_owned`, `IrohRpc::bind/bind_on/bind_owned`,
  `RecoverySession::host/host_on/host_in`, `join/join_in`; only `new_owned`, `bind_owned`, `host_in` and
  `join_in` are used outside tests, and `host_in(.., server_owned: bool)` / `join_in(.., host, .., bool)`
  rebuild an `Ownership` that the caller could pass.
- Proposed change: give `Coordinator` one `pub fn store(&self) -> &Store` (today test-only) and delete its
  forwards; delete `RecoverySession`'s forwards (callers use `recovery.coordinator`). Replace both
  `probe_reserved` signatures with `probe_reserved(&ProbeReservation) -> bool` implemented as
  `machine.probes.contains(reservation)`. Collapse each constructor family to one taking `Ownership`
  (`RecoverySession::host(room, primary, relay_only, port, Ownership)`,
  `join(room, primary, host_coordination, relay_only, Ownership)` where `Ownership::Member { host }` carries
  the host incarnation); give tests a helper.
- Behavior preserved: everything; pure API reshaping.
- Behavior at risk: none identified.
- Estimated saving: ~150 lines and 19 forwarding methods deleted, two clippy allows removed, one naming
  drift removed.
- Confidence: high

### F-COORD-8: coordination.rs (1635 lines) and recovery.rs (1223) each hold four or five unrelated concerns
- File: rust/sf4-net/src/coordination.rs:71
- File: rust/sf4-net/src/coordination.rs:319
- File: rust/sf4-net/src/coordination.rs:617
- File: rust/sf4-net/src/coordination.rs:990
- File: rust/sf4-net/src/recovery.rs:730
- File: rust/sf4-net/src/recovery.rs:896
- File: rust/sf4-net/src/coordination/latency_roster.rs:189
- Category: size
- Severity: P2
- Claim: coordination.rs mixes wire codecs, the Raft state machine and storage, a JSON validator for the
  native checkpoint, the OpenRaft network adapter, and the coordinator with its RPC dispatcher. recovery.rs
  mixes the session lifecycle, membership operations, successor and departure proofs, the native checkpoint
  transfer framing, and ping statistics (delay bands, percentile summaries) that have nothing to do with
  recovery: recovery.rs:973 says "Match probe.rs's displayed p50" and probe.rs:255 repeats the same
  percentile index expression. latency_roster.rs carries a `#[cfg(test)] impl Store` (189-260) in a
  non-test file.
- Proposed change: seams for coordination.rs:
  `coordination/wire.rs` (CompressedJsonWire, InstallSnapshotWire, encode/decode_snapshot, 71-162 and
  423-477, the home of F-COORD-2); `coordination/store.rs` (Machine, Memory, Store, RaftLogReader,
  RaftSnapshotBuilder, RaftStorage, 319-988); `coordination/committed_view.rs` (committed_probe_pair_bound
  617-714 merged with latency_roster.rs per F-COORD-5); `coordination/network.rs` (RpcTransport, Network,
  Peer, 990-1072); coordination.rs keeps Coordinator + dispatch (~550 lines). Move the test-only `impl Store`
  into coordination/tests. Seams for recovery.rs: probe math (ProbeResult, summarize_probe,
  summarize_datagram_probe, recommended_delay, delay_band, PROBE_* constants, 896-1019 and 954-990) into
  probe.rs with one shared `percentile(sorted, pct)`; CheckpointTransfer, IncomingTransfer, parse_digest,
  hex_digest, sha256 (730-943) next to their only consumer, service/checkpoints.rs, or `recovery/transfer.rs`;
  `confirm_successor`, `confirm_available_successor`, `confirm_departure`, `confirm_singleton_departure`,
  `first_confirmed_successor` into `recovery/departure.rs` with one `AuthorityClaim::is_leader_at(id, term,
  revision)` predicate shared with `Coordinator::read_barrier` (four copies of the same four comparisons today).
- Behavior preserved: everything; moves only.
- Behavior at risk: none identified.
- Estimated saving: no file over ~600 lines; one percentile rule instead of two; one claim predicate
  instead of four.
- Confidence: high

### F-COORD-9: The RPC method is a string matched in five parallel tables
- File: rust/sf4-net/src/coordination_iroh.rs:174
- File: rust/sf4-net/src/coordination_iroh.rs:632
- File: rust/sf4-net/src/coordination_iroh.rs:745
- File: rust/sf4-net/src/coordination.rs:1473
- File: rust/sf4-net/src/coordination/server_owned.rs:49
- Category: boundary
- Severity: P2
- Claim: `method: &'static str` crosses the `RpcTransport` trait and is matched independently in
  `request_ceiling`, the serve-side u8->str table, the call-side str->u8 table (with a runtime
  "unknown method" error path that cannot happen), `Coordinator::dispatch`, and `Ownership::serves`.
  `serve` also special-cases `"authority"` and `"remove"` by string, and `call` special-cases `"authority"`
  for connection-guard behavior. Adding a method means editing five places in two files with no compiler help.
- Proposed change: `#[repr(u8)] enum Method { Append = 1, Vote, Snapshot, Authority, Propose, Elect, Remove }`
  with `fn ceiling(self)`, `fn mutates_raft(self)` (for the retired-route rule), `fn keeps_connection_on_cancel(self)`;
  `RpcTransport::call(.., method: Method, ..)`, `dispatch(source, Method, &[u8])`,
  `Ownership::serves(source, Method)`. Same byte codes on the wire.
- Behavior preserved: wire bytes, every check.
- Behavior at risk: none identified.
- Estimated saving: four string tables and one impossible error branch deleted; exhaustive matches.
- Confidence: high

### F-COORD-10: Proposal validity rules are written three times
- File: rust/sf4-net/src/coordination.rs:1397
- File: rust/sf4-net/src/coordination.rs:1577
- File: rust/sf4-net/src/coordination.rs:869
- File: rust/sf4-net/src/coordination.rs:820
- Category: spaghetti
- Severity: P3
- Claim: the request/dedup/checkpoint size rules appear in `Coordinator::propose` (1397-1404), the "append"
  pre-check in `dispatch` (1577-1583, a subset: no dedup check), and the accept branch of
  `apply_to_state_machine` (869-877, plus term/base rules); the probe-reservation validity is written in
  apply (821-829) and again, slightly differently, in `install_snapshot` (955-962: checks `term != 0`, apply
  does not check `pair_revision`, neither does install) and `RecoverySession::reserve_probe` (620-627).
  `apply_to_state_machine` is a ~100-line nested match with an early `continue` that skips the shared
  `receipts.push` at the end of the loop body.
- Proposed change: `Proposal::well_formed(&self) -> bool` and `ProbeReservation::well_formed(&self) -> bool`,
  used by all sites; split apply into `Machine::apply_membership`, `Machine::apply_probe`,
  `Machine::apply_checkpoint`, each returning a `Receipt`.
- Behavior preserved: keep each site's extra rules (term, base) where they are; only the shared shape rules
  move. Verify the differences above are intended before unifying (they look accidental).
- Behavior at risk: if the probe differences are intended, unifying would change what a snapshot accepts.
- Estimated saving: ~40 lines; one place to change limits.
- Confidence: medium

### F-COORD-11: Authority polling runs a full codec on tiny heartbeats about 30 times a second on a 16-member leader
- File: rust/sf4-net/src/coordination.rs:1233
- File: rust/sf4-net/src/coordination.rs:63
- File: rust/sf4-net/src/recovery.rs:407
- File: rust/sf4-net/src/service/mod.rs:1497
- Category: overhead
- Severity: P3
- Claim: `emit_coordination_state` runs on the actor's 1 s statistics interval (service/mod.rs:1497-1500) and
  calls `RecoverySession::state`. A follower's `state` sends an "authority" RPC to the leader
  (`read_barrier`), so a 16-member leader serves ~15 claims/s, each a new QUIC bi-stream with a JSON
  `AuthorityClaim` reply (~300-600 B with the voter and member sets). `AUTHORITY_CLAIM_COALESCE` is 500 ms,
  shorter than the 1 s poll period, so with followers at random phases the leader runs
  `ensure_linearizable` about twice a second; each one heartbeats the replication targets, and each empty
  heartbeat still goes through `CompressedJsonWire::encode` (JSON, zlib, blake3, base64, JSON again) for a
  ~200 B body. This is the only timer-driven recurring send in the area. It is small in bytes (order of
  10-20 KB/s on the leader) but it is a steady stream of codec work and stream setups.
- Proposed change: no cadence change (the claim is a freshness proof). Remove the per-heartbeat codec cost
  with F-COORD-2 (empty appends become a few dozen raw bytes). Optionally align the coalesce window with the
  poll period (e.g. 1 s, as the host already uses 2.5 s), which halves leader barriers; that is a freshness
  trade-off for the owner to decide, not behavior-preserving by itself.
- Behavior preserved: poll cadence, claim content, writable semantics.
- Behavior at risk: none for the codec change; the coalesce change lengthens how stale a shared claim may be.
- Estimated saving: ~30 zlib+blake3+base64 runs per second on the leader removed; ~25% of heartbeat bytes.
- Confidence: medium (OpenRaft 0.9 heartbeat fan-out per barrier not traced into the crate)

### F-COORD-12: Superseded full-checkpoint entries are replayed to a lagging follower one RTT at a time
- File: rust/sf4-net/src/coordination.rs:1132
- File: rust/sf4-net/src/coordination.rs:1138
- Category: streaming
- Severity: P3
- Claim: every `Normal` entry is a whole-state replacement, yet `max_payload_entries: 1` and snapshots only
  every 3 (host) or 8 (private) logs mean a follower k entries behind downloads k full checkpoints, one RTT
  each, all but the last immediately overwritten. The log also keeps up to 8-10 full checkpoints in memory
  per helper (the config comment says so).
- Proposed change: after F-COORD-1/F-COORD-4 make snapshots cheap (no recompression, no lock hold), lower
  `SnapshotPolicy::LogsSinceLast` and `max_in_snapshot_log_to_keep` so catch-up becomes one snapshot.
  Do not change the heartbeat or append TTL.
- Behavior preserved: committed state and order.
- Behavior at risk: catch-up timing and the snapshot path being exercised far more often (it is the less
  tested path today); needs the IrohRecoveryIntegrationTest suite.
- Estimated saving: for a follower 3 behind, ~2 x 90 KB compressed and 2 RTTs per catch-up; up to ~3 MB
  steady memory per private-room helper.
- Confidence: low

### F-COORD-13: transport.rs repeats its connection guard seven times and classifies errors by string
- File: rust/sf4-net/src/transport.rs:237
- File: rust/sf4-net/src/transport.rs:298
- File: rust/sf4-net/src/transport.rs:328
- File: rust/sf4-net/src/transport/with_proof.rs:24
- Category: judo
- Severity: P3
- Claim: `let mut pending = PendingConnection(Some(c.clone())); let result = timeout(..).await..; if
  result.is_ok() { pending.0 = None }` appears in `connect_control_at`, `connect_control_on_proof`,
  `accept_control_policy`, `accept_game_stream_until`, `accept_game_stream_with_until`, `accept_control_with`,
  `connect_public_on_proof` (and test-only `accept_game`). There are eight `connect_control*` entry points
  (`connect_control`, `_with`, `_at`, `_on`, `_on_expected`, `_to`, `_to_with`, `_on_proof`), several one-line
  forwards. `is_host_unreachable`, `is_admission_refused` and `is_handshake_timeout` compare
  `error.to_string()` with a constant, so the error kind is carried in display text.
- Proposed change: `async fn guarded<T>(c: &Connection, deadline, fut) -> io::Result<T>` that closes on any
  non-Ok; collapse the control dialers to `connect_control(endpoint, address, expected, proof, Mode)`;
  carry the three outcomes as a small `HandshakeError` enum inside `io::Error::other` and downcast, or return
  a typed error from these functions.
- Behavior preserved: close reasons, timeouts, error text sent to native.
- Behavior at risk: none identified if the display strings stay the same.
- Estimated saving: ~80 lines; string-matched error classification removed.
- Confidence: high

### F-COORD-14: invite.rs decodes the same header twice and dispatches prefixes twice
- File: rust/sf4-net/src/invite.rs:220
- File: rust/sf4-net/src/invite.rs:262
- File: rust/sf4-net/src/invite.rs:283
- File: rust/sf4-net/src/invite.rs:145
- Category: judo
- Severity: P3
- Claim: `parse` and `rejection_reason` each carry their own prefix dispatch over the five text forms;
  `decode_compact` and `decode_recovery_compact` duplicate the 91-byte header parse and differ only by the
  48-byte recovery tail; `encode_discord` calls `encode`, base64-decodes the result and re-encodes the first
  91 bytes.
- Proposed change: `fn decode_text(text, local_build) -> Option<Invite>` used by `parse`,
  `parse_for_build` and `rejection_reason`; one `decode_header(&[u8]) -> (fields, rest)` plus an optional
  recovery tail; `fn compact_bytes(&self) -> Vec<u8>` shared by `encode` and `encode_discord`.
- Behavior preserved: every accepted and rejected text, including sf4e1 legacy JSON and emd1/emd2.
- Behavior at risk: none identified (cross-version text formats unchanged).
- Estimated saving: ~70 lines.
- Confidence: high

### F-COORD-15: Body framing copies through a scratch chunk
- File: rust/sf4-net/src/coordination_iroh.rs:671
- Category: overhead
- Severity: P3
- Claim: `read_sized` reads each 16 KB piece into a scratch `chunk` and then `extend_from_slice`s it into
  `data`, so every received coordination byte is copied twice; the response path (`read_body`) does the same.
- Proposed change: grow `data` by `min(remaining, CHUNK)` with `resize` and `read_exact` into the new tail;
  memory still follows the bytes received.
- Behavior preserved: the same bound and allocation-follows-bytes property.
- Behavior at risk: none identified.
- Estimated saving: one memcpy of every append and snapshot body on receive (~100 KB per follower per commit).
- Confidence: high

## Area summary
- Health: the Raft layer is careful about bounds, fencing and authentication, and the files are well
  commented, but the coordination wire was built for safety, not cost: one whole checkpoint per entry,
  encoded per follower, wrapped JSON-in-base64-in-JSON, copied at every hand-off.
- Biggest judo move: compress the checkpoint once at `propose` and carry `Arc` bytes through the log,
  the append wire, the committed value and the snapshot (F-COORD-1 + 2 + 3), which also deletes
  `CompressedJsonWire`, the base64 in `InstallSnapshotWire`, and most of `encode_snapshot`'s work.
  Structurally, delete the Store -> Coordinator -> RecoverySession pass-throughs and telescoping
  constructors (F-COORD-7) and split both large files on the named seams (F-COORD-8).
- Biggest overhead item: F-COORD-1, about 15 identical compress+encode passes per commit on a
  16-member leader (16 per room on a public host), on the workers that gameplay bridges share; not
  covered by RoomHostBench because it is network-free.
- Negative result for the consolidator: nothing in this area sends a large JSON on a 15 s timer. Appends
  are per commit; the Raft heartbeat is 9 s and empty; the "15 s" values here are timeouts
  (coordination.rs:1123 replacement offer, IrohRoomCheckpoint.cxx 15000 ms proposal abandon).
  The only recurring send is the 1 Hz authority poll (F-COORD-11), which is small.
- Not assessed: OpenRaft 0.9.25 internals (exact heartbeat fan-out per `ensure_linearizable`), real
  checkpoint byte composition (quote density, compression ratio), and the native side beyond the
  checkpoint staging path. All millisecond figures are estimates, not measurements.
