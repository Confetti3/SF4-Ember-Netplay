# Adversary verdicts: rust-coordination.md (F-COORD-1..15) and rust-service.md (F-SVC-1..14)

Checked read-only at sf4-nightly b9ce0e31. OpenRaft 0.9.25 and iroh-base 1.1.0 sources read from the local cargo registry.

## Shared compatibility evidence (applies to F-COORD-1, F-COORD-2, and the ALPN bump)

- The same-build gate compares `proof.build` (invite.rs:493). That value is `sf4e::sidecarHash`, the SHA-256 of
  **Sidecar.dll**, from `GetHash(hinstDll, ...)` (src/sf4e/sf4e.cxx:46, :232). It does **not** cover sf4-net.exe.
  So "no coordination group ever mixes builds" holds for DLL bytes only. Two packages with the same Sidecar.dll and
  different helpers count as the same build: for example a helper-only hotfix, or an incremental build where no
  native source changed, so the DLL was not relinked and its PE timestamp and version resource stayed the same.
  Today this is harmless because the Raft wire never changes without a DLL change. A Raft wire change makes it a
  real gap. Private rooms are protected only by the fact that one package carries both binaries; no code enforces
  it.
- Public rooms: the supervisor routes by `build_id`, and the room host plus its sf4-net are staged as a pair under
  `builds/<build_id>/`, both built from one commit (server/roomhost/README.md:9-10, :67-113;
  PUBLIC_ROOMS.md:328). A host that lags the client shows up as `unsupported_build`, and a host that leads is a
  different build_id. So clients never mix wires with a host, **provided the staging rule is followed**. That is a
  procedural dependency, not a code guarantee: restaging `builds/<id>/sf4-net` alone from another commit would
  break every room of that build.
- Required adjustment for any Raft wire change: bump the ALPN `sf4e/coordination/1` (coordination_iroh.rs:27), **and**
  make the helper's wire version part of the identity it compares. One way is to have the helper append a
  coordination wire version to the build string it checks in invites and proofs. A mismatch then fails at
  invite/admission with "incompatible" instead of at the coordination handshake after the control link is up.
- Snapshots are in memory only (the Store is process-incarnation scoped, coordination.rs:1-6; no disk persistence)
  and travel only inside the group. A `SNAPSHOT_MAGIC` bump (`sf4rs001` to `sf4rs002`, coordination.rs:62) is
  therefore safe under the group rule, and it is **required** for any change to `Committed`'s serialized shape.
- The helper-to-native IPC has two native consumers: the game's Sidecar.dll, and the Linux room host, which compiles
  the same IrohRoom*.cxx (server/roomhost/build-linux.sh:69-73). Both ship from the same commit as their helper, so
  an IPC change is a same-commit change on both sides.

---

### F-COORD-1: PLAUSIBLE  (severity: keep P1 for public room hosts, P2 in private rooms)
- Decisive invariant/evidence: Each follower's `Peer` encodes its own request:
  `CompressedJsonWire::encode(&request)` at coordination.rs:1047, from an `Entry` cloned per read (:720-727). OpenRaft
  makes one replication stream per target. Checkpoint proposals are leader-only ("obsolete room authority",
  coordination.rs:~1406), so compressing at proposal time is possible. The entry type is
  `declare_raft_types!(... D = Proposal ...)` (:66), so the change alters the serialized `Entry` payload, which is
  replicated state.
- Adjustment:
  1. Ship the no-wire-change memo first. One cache in `Network`, shared by every `Peer`, keyed by
     (vote, prev_log_id, leader_commit, entry LogIds). An entry is immutable per LogId, so this key is sound.
     Bound it to the last one or two requests. Up-to-date followers share the key, so this removes most of the N-1
     encodes with zero compatibility risk.
  2. The full change is not self-contained. JSON cannot carry `Arc<[u8]>`: serde_json writes it as a number array,
     about 3-4x larger. So it **requires** F-COORD-2's binary codec plus a custom Serialize/Deserialize for
     `Proposal`, and a binary or custom snapshot encoding with the magic bump. Committed's shape changes too.
  3. Apply must keep today's accept/reject. A `String` checkpoint is guaranteed UTF-8 and bounded today
     (coordination.rs:875, :1580). Inflate and validate the declared raw length and UTF-8 in the "append" decode path
     (dispatch, :1577), outside the store mutex, and carry the raw `Arc<str>` beside the bytes. Inflating inside
     `apply_to_state_machine` would hold the store lock for 1-2 ms per commit.
  4. Every member still needs the raw text: `Committed::checkpoint()` is read by observe_commit, the latency roster,
     probe_pair_bound, and the pump. So followers save only the JSON unescape, not an inflate.
  5. Keep `Proposal::digest()` (:189) over the raw text, so the dedup digests in `recent` do not depend on the
     compressor build.
  6. Compress inside the spawned propose task, not on the actor. `recovery.propose` runs on the actor
     (checkpoints.rs:~250).
  7. Committed-value watchers (`commits`/`observed`, :398-416) key on revision and count, so they are unaffected.
- Duplicate of: no. Canonical for "compress once / per-follower encode". F-COORD-2 and F-COORD-3 are its parts.

### F-COORD-2: PLAUSIBLE  (severity: keep P2)
- Decisive invariant/evidence: Append bodies are JSON, then zlib, then base64 into `CompressedJsonWire.data: String`,
  then JSON again (coordination.rs:73-128, :1031). The snapshot is base64 in `InstallSnapshotWire` (:130-160), and
  body framing is already u32-length binary (coordination_iroh.rs:659-703). Dropping blake3 is fine: zlib's adler32
  covers the inflated bytes and QUIC's AEAD covers transit.
- Adjustment: This is a wire change, so it needs the compatibility adjustment above (ALPN plus a helper wire version in
  the compared build identity; the DLL-hash gate alone is insufficient). Keep the declared-length check, the
  `total_in` check and the `take(MAX_SNAPSHOT+1)` bound. Re-derive `MAX_SNAPSHOT_REQUEST` (128 KiB) and `MAX_RPC`
  ceilings for the smaller bodies (or leave them as they are, since they are only upper bounds).
- Duplicate of: no (it is the codec half of F-COORD-1).

### F-COORD-3: CONFIRMED  (severity: keep P2)
- Decisive invariant/evidence: serde's `rc` feature is enabled (Cargo.toml:31), so `Arc<str>` serializes as a JSON
  string. Entry, append and snapshot bytes therefore stay identical with no wire or `SNAPSHOT_MAGIC` change. The
  copies are real: entry clone (:725), `proposal.checkpoint.clone()` in apply (:884), `String::from_utf8(bytes.clone())`
  (recovery.rs:593), and `to_vec` in the pump (checkpoints.rs:300).
- Adjustment: Also pass the cached `Committed::digest()` into the transfer. `CheckpointTransfer::new` recomputes
  SHA-256 over about 460 KB (recovery.rs:759) on the actor thread for every commit, even though
  `RecoverySession::state()` already computes and caches the same digest every second (recovery.rs:~409). That is a
  per-commit actor cost both reviewers missed. `same_commit` keeps working (Arc<str> compares by content).
- Duplicate of: no. Canonical for the copy-and-share items. It absorbs F-SVC-14's first half.

### F-COORD-4: CONFIRMED  (severity: keep P2)
- Decisive invariant/evidence: `build_snapshot` holds `self.0.lock()` across `encode_snapshot` (coordination.rs:731-745).
  The actor's pump takes the same tokio Mutex on every tick (checkpoints.rs:271, coordination.rs:492). Snapshot builds
  in OpenRaft 0.9 are **spawned** (openraft core/sm/worker.rs:89-93, :188-191), so they overlap apply and
  install_snapshot.
- Adjustment: The finding's claim "OpenRaft serializes snapshot builds itself" is not a sufficient safety argument.
  With clone-encode-relock, an `install_snapshot` that lands between the clone and the re-lock would be overwritten by
  the older built snapshot in `memory.snapshot`. Store the new snapshot only if its `last_log_id` is at least that of
  the snapshot already stored.
- Duplicate of: no. Related to F-SVC-7, which touches the same mutex from the reader side.

### F-COORD-5: DUPLICATE  (severity: P2)
- Decisive invariant/evidence: Both Value walks are real (latency_roster.rs:74, coordination.rs:640). F-SVC-4 covers
  them plus the third walk in seats.rs:180, `observe_commit`.
- Adjustment: The saving is overstated. A skip-scan with IgnoredAny still validates every string, so expect about
  1-2 ms saved per Value parse of about 460 KB, not "3-6 ms down to under 1 ms". See F-SVC-4 for the typed-view
  caveats.
- Duplicate of: F-SVC-4 (canonical)

### F-COORD-6: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: The pump and `completed_checkpoint_proposal` export the leader's own commit back to its
  native side (checkpoints.rs:331, :585). Native clears `proposalBytes_` only after the round trip
  (IrohRoomCheckpoint.cxx:124-126). The helper proposes the exact transfer bytes (recovery.rs:593), and the match key
  includes length and digest, so `proposalBytes_` is byte-identical to the commit.
- Adjustment: The saving is leader-only and stays on the local pipe; native decode (the 11 ms) is unchanged. Native
  clears `proposalBytes_` on a term change and on any checkpoint-scope error (IrohRoom.cxx:429, :714-716), so the
  fallback to a full transfer is mandatory, not optional. The marker must take the transfer's slot in the same
  `pending_checkpoint_committed` ordering. The Linux room host is the public-room leader and shares this native code,
  so it benefits most. If F-SVC-14's binary IPC lands, the remaining saving is smaller.
- Duplicate of: no (it is the leader special case of F-SVC-14's IPC encoding).

### F-COORD-7: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: Forwarders at coordination.rs:1285-1404 and recovery.rs:454+. `probe_reserved` has seven
  scalars with a clippy allow (:563). Outside tests only `new_owned`, `bind_owned`, `host_in` and `join_in` are
  constructed. `host_in(.., server_owned: bool)` rebuilds `Ownership` (recovery.rs:100-111).
- Adjustment: Severity drops to P3: this is pure API reshaping with no runtime effect.
- Duplicate of: no

### F-COORD-8: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: The seams are as described. This is moves only.
- Adjustment: P3 (no overhead effect). Do it after F-COORD-1/2/3 so the codec moves once.
- Duplicate of: no

### F-COORD-9: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: Five string tables: `request_ceiling` (coordination_iroh.rs:174), serve u8 to str (:632),
  call str to u8 (:745, with an unreachable `_ => Err`), dispatch, and `Ownership::serves`. The byte codes 1-7 stay the
  same.
- Adjustment: none
- Duplicate of: no

### F-COORD-10: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: Apply requires `reservation.term == proposal.term == entry term`, which is non-zero for
  a real leader term, so it is equivalent in practice to install's `term != 0` (coordination.rs:821-829 vs :955-962).
  No site checks `pair_revision`.
- Adjustment: Unify only the predicates that are identical today. Adding `pair_revision != 0` or a dedup rule to
  apply or install changes which replicated entries and snapshots are accepted. That is deterministic within a build,
  but it is a behavior change and should not ride on a refactor.
- Duplicate of: no

### F-COORD-11: REJECTED  (severity: P3)
- Decisive invariant/evidence: OpenRaft's linearizable barrier sends empty appends to **voters only**
  (openraft core/raft_core.rs:322-325 `is_voter` filter). It returns without any RPC when the leader alone is a quorum
  (:314-316), which is the public host as sole voter. `MAX_VOTERS = 5` (recovery.rs:26). At about 2 barriers per
  second to at most 4 voters, that is about 8 encodes of about 200 B each per second, microseconds of CPU, not "about
  30 runs a second". The roughly 15 authority RPCs per second on a 16-member leader are real but small.
- Adjustment: Nothing to do beyond F-COORD-2. Changing the coalesce window alters claim freshness and is not
  behavior-preserving.
- Duplicate of: no

### F-COORD-12: REJECTED  (severity: P3)
- Decisive invariant/evidence: The config comment (coordination.rs:1124-1136) chooses 8 retained logs on purpose, "to
  serve a slow voter". Lowering it sends more catch-ups down the snapshot path: 64 KiB windows, plus an `observations`
  bump that the latency Observer treats as a replaced commit or a skip (latency/authority.rs:~220). That changes
  catch-up timing on the less-tested path, for a speculative gain (only when a follower is 3 or more behind).
- Adjustment: Revisit only with measurements after F-COORD-1/3/4.
- Duplicate of: no

### F-COORD-13: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: Display-string classification (transport.rs:298-326) and the repeated
  `PendingConnection` guard.
- Adjustment: The display strings reach native as error text, so keep them byte-identical.
- Duplicate of: no. Related to F-SVC-13 (the same stringly-error pattern in service/).

### F-COORD-14: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: Duplicate header parsing and prefix dispatch (invite.rs:220, :260, :280, :307).
  `encode_discord` re-decodes the output of `encode` (:144-155).
- Adjustment: Invite texts are the one truly cross-version surface. Before the refactor, add golden accept/reject
  tests for sf4e1, sf4e2, sf4e3, emd1 and emd2. Preserve the deliberate truncation in `encode_discord`, which keeps
  only `bytes[..91]` even for recovery invites.
- Duplicate of: no

### F-COORD-15: REJECTED  (severity: P3)
- Decisive invariant/evidence: This is one memcpy of about 100 KB per received body (coordination_iroh.rs:671-683), a
  few microseconds. The proposed `resize` zero-fills, which replaces the copy with a memset of the same size.
- Adjustment: none (negligible)
- Duplicate of: no

---

### F-SVC-1: PLAUSIBLE  (severity: P2; the grace-pump sub-claim is a real but benign-impact bug)
- Decisive invariant/evidence: The structural claim is accurate. `clear_room` (mod.rs:861-918) and
  `enter_departure_grace` (departure.rs:582-623) are near-identical hand-kept lists. Grace keeps `recovery`, `room`
  and `last_exported_revision`, and `pump_committed_checkpoint` has no grace gate (checkpoints.rs:265-275, called
  every tick at mod.rs:1493). The finding's example is wrong, though: the leader committing this member's removal is
  an `EntryPayload::Membership` entry, which never changes `committed` or its revision (coordination.rs:~806-811), so
  it never triggers an export. The real trigger is any *Normal* commit (another table's Ready, chat, or the native
  departure snapshot) applied while this helper is still in Raft membership. That happens on the abandon path, where
  `abandon_room` enters grace with its vote retained on purpose (departure.rs:631-645), and in the window before
  removal.
  What native does next: after `room_closed` its state is Idle (IrohRoom.cxx:838-842). At :864 Idle skips both
  `ConsumeCoordinationEvent` and error handling, so the `checkpoint_begin` is never acked. The helper then sits at
  FAST_TICK (2 ms) because `outgoing_transfer` counts as in flight (mod.rs:764-779), for up to 30 s. After 15 s it
  times out, emits `checkpoint_send_timeout` (ignored by native), lowers `last_exported_revision` and retries once
  more before grace ends. The cost is wasted copy, SHA-256, base64 and a 500 Hz tick. There is no state corruption,
  because a native `Begin` resets everything (IrohRoom.cxx:59) and the helper's `begin()` clears the grace.
- Adjustment: Fix the grace pump separately, as a deliberate one-line behavior change with a test: return early from
  `pump_committed_checkpoint` while `retirement_started.is_some()`. Grace exists only to answer authority RPCs. Then
  do the `RoomState`/`Phase` refactor as a pure move that reproduces today's survivor list exactly (`room`,
  `recovery`, `epoch`, `short`, `refresh_roster`, `seats`, `join_*`, `departure_failed`, `closed_generation`,
  `last_exported_revision`, `latency`). Releasing `refresh_roster` at grace start is safe: refresh.rs:107 returns
  early in grace. Severity drops from P1, since the refactor has no runtime effect.
- Duplicate of: no

### F-SVC-2: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: The seams are as listed. This is moves only.
- Adjustment: P3 (no overhead effect).
- Duplicate of: no

### F-SVC-3: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: `run()` sends every `Command::Leave` to `leave_command` (mod.rs:1418-1422) before the
  catch-all `command()` (:1423). `Actor::command` has no other production caller; grep finds only tests (tests.rs:3972,
  :3991, and the latency, match_watch and public_rooms tests). The Leave arm at mod.rs:997-1005 is therefore
  production-dead. Production Leave goes through `graceful_leave` (departure.rs:35+).
- Adjustment: The tests at tests.rs:3963-4000 build an Actor and call the sync `command()` with no `commands` or
  `failed_ipc` channels. Moving them to the real path means building those channels and calling `leave_command`. With
  no recovery the graceful path still clears the room and emits `RoomClosed`, so the asserts should hold.
- Duplicate of: no

### F-SVC-4: PLAUSIBLE  (severity: P2, down from P1)
- Decisive invariant/evidence: Each commit gets two full `Value` parses: `observe_commit` (seats.rs:180, cached by
  `same_commit`) and `committed_latency_roster` (latency_roster.rs:74, cached by (revision, term, digest)). A probe
  check adds more (coordination.rs:640 via probes.rs:189, :261, :520). `observe_commit` runs **on the actor**, from
  the pump and the refresh completion.
- Adjustment: Expect about 1-2 ms saved per parse, not more. The typed view must reproduce the Value leniency
  exactly:
  - `as_i64` for table ids vs `as_u64` for member ids (latency_roster.rs:95, :104)
  - absent-or-wrong-type means None
  - the two endpoint key spellings
  - last-wins on duplicate keys (a serde derive errors on a duplicate field)
  - borrowed fields as `Cow<str>`, not `&str` (escaped strings cannot borrow)
  - every case that today yields `RosterInvalid` keeps yielding it
  Keep `Seats::read` on a small `Value` subtree. No wire change.
- Duplicate of: no. Canonical; F-COORD-5 folds in here.

### F-SVC-5: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: `last_control_rebound` is only ever assigned (refresh.rs:418, nine resets, the field at
  mod.rs:692), and no read exists anywhere. Step 1 is safe; remove the field from entry.rs:135 and tests.rs:270 too.
  Native confirms why the 1 s resend is load-bearing: rebound is cleared on a term change (IrohRoom.cxx:429) and on
  ControlLeader-scope errors (:721-722).
- Adjustment: Step 2's invalidation list is incomplete, so step 2 is unsafe as written. Native also clears rebound in
  three other places:
  - on the leader's `control_closed`, which has no Error event (IrohRoomPeers.cxx:216-217)
  - in `Fail()` (IrohRoom.cxx:35)
  - by silently dropping a rebound whose `leader` differs from `coordination_.leader` (IrohRoom.cxx:445)
  The code comment's IPC-reader race (refresh.rs:403-408) is a fourth reason. The saving is about 2.5 KB/s on a local
  pipe. Do step 1 only.
- Duplicate of: no

### F-SVC-6: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: The payload is identical for every peer from the pump: `retired_before_retry` is empty
  (controls.rs:350-372) and `membership_view` does not depend on the peer (server_owned.rs:51). The leader republishes
  on every refresh while `pending_retired_incarnations` is non-empty (refresh.rs:215, :239). Receivers re-run
  provenance and admission work (controls.rs:537-600).
- Adjustment: Step 1 (serialize once) is safe. Steps 2 and 3 remove the implicit retry that a follower's failed
  admission operation relies on, so keep a forced republish if they are done. The windows only last for a pending
  retirement (at most about 12 s, and rare), so the impact is low.
- Duplicate of: no

### F-SVC-7: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: `recovery.committed().await` locks the store on every tick (checkpoints.rs:271,
  coordination.rs:492). FAST_TICK is 2 ms (mod.rs:55). A commit watch already exists and is only forwarded to `wake`
  (refresh.rs:95-103).
- Adjustment: Compare against the cached latest revision, not `has_changed()`, because the timeout path lowers
  `last_exported_revision` with no watch change (checkpoints.rs:391-393). Uncontended acquisitions are cheap; the
  value is in not queueing behind apply or the snapshot build (see F-COORD-4). Hence P3.
- Duplicate of: no. Related to F-COORD-4.

### F-SVC-8: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: `RawValue::from_string(self.payload.clone())` (protocol.rs:567). Borrowing with
  `from_str::<&RawValue>` gives the same output, whitespace trimming included. `kind: String` is built per send
  (mod.rs:1031).
- Adjustment: Phase 1 is safe. Phase 2 changes the IPC contract for both native consumers (the game DLL and the Linux
  room host, from the same commit), so no dual-form transition is needed if both ship together. The saving is small.
- Duplicate of: no

### F-SVC-9: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: iroh-base 1.1.0 `EndpointAddr` derives PartialEq and Eq with a `BTreeSet` of addresses
  (endpoint_addr.rs:41-47), and `EndpointId` is PartialEq. So comparing fields matches comparing the deterministic
  serialized bytes (members.rs:334).
- Adjustment: none
- Duplicate of: no

### F-SVC-10: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: There are six id-bump sites. Only `send_membership_control` bumps on success
  (controls.rs:329-339); the others bump first (controls.rs:26, :122; mod.rs:1043; probes.rs:53;
  server_owned.rs:146). The marker is read positionally (refresh.rs:238, short_links.rs:339).
- Adjustment: none
- Duplicate of: no

### F-SVC-11: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: `state.leader` holds the coordination id (`BasicNode.addr` =
  `coordination_endpoint.to_string()`, recovery.rs:121, :295) and is then overwritten with the primary id
  (refresh.rs:294-304). iroh-base serializes `PublicKey` as its lowercase-hex Display string for human-readable
  formats (key.rs:78-90, :221-224), so the IPC JSON stays identical.
- Adjustment: Keep `""` for an absent leader on the wire. The saving (about 100 small allocations per second) is
  negligible, so treat this as a clarity change.
- Duplicate of: no

### F-SVC-12: REJECTED  (severity: P3)
- Decisive invariant/evidence: `is_retired` is an uncontended RwLock read on `rpc.members`, not the store mutex
  (coordination_iroh.rs:492-498). It runs in a spawned task off the actor (refresh.rs:137-160). About 20 of these per
  second costs microseconds. "Skip apply when unchanged" puts time-driven work at risk: `unwritable_leader_since`
  grace, admission retries (refresh.rs:250-290), and the ControlRebound resend that F-SVC-5 shows is load-bearing.
- Adjustment: Batching `is_retired` is harmless but not worth a finding.
- Duplicate of: no

### F-SVC-13: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: probes.rs:275-280 matches on `error.to_string()`; network.rs:126 compares with
  `"relay_unavailable"`.
- Adjustment: Keep the probe failure codes 1-11 and the event JSON byte-identical (native reads `probe_failure`).
- Duplicate of: no. Related to F-COORD-13.

### F-SVC-14: PLAUSIBLE  (severity: keep P3)
- Decisive invariant/evidence: The pump copies the checkpoint (`to_vec`, checkpoints.rs:300), re-hashes it
  (recovery.rs:759), and base64-encodes 16 KB chunks into JSON (checkpoints.rs:497). IPC frames are length-framed
  binary (wire.rs:36-82), so a binary chunk frame is feasible.
- Adjustment: The shared-bytes and Arc part duplicates F-COORD-3. Add reusing `Committed::digest()` (see F-COORD-3).
  The binary-chunk part changes the IPC for both native consumers, from the same commit. Keep the credit window and
  the `native_json_depth_ok` gate (checkpoints.rs:16).
- Duplicate of: partly. The copy half belongs to F-COORD-3 (canonical). The base64 half is canonical for IPC
  encoding, and F-COORD-6 is its leader special case.

## Batch verdict

- Counts (29 findings):
  - CONFIRMED 14: F-COORD-3, 4, 7, 8, 9, 13, 14; F-SVC-2, 3, 7, 9, 10, 11, 13
  - PLAUSIBLE 10: F-COORD-1, 2, 6, 10; F-SVC-1, 4, 5, 6, 8, 14
  - REJECTED 4: F-COORD-11, 12, 15; F-SVC-12
  - DUPLICATE 1: F-COORD-5, folded into F-SVC-4. F-SVC-14 is also partly a duplicate of F-COORD-3.
- Do first:
  1. **The F-COORD-1 memo plus F-COORD-3 (`Arc<str>`, shared transfer bytes, reuse of the cached digest).** No wire
     change and no snapshot change. Together they remove most of the per-follower encode on the public room host
     (16 learners per room, many rooms per VPS) and a per-commit SHA-256 and 460 KB copy on every actor.
  2. **F-COORD-4 (snapshot encode outside the lock, with the newest-snapshot guard) together with F-SVC-7.** This
     removes 3-5 ms actor stalls behind the store mutex.
  3. **F-SVC-4 (one typed view per commit, on the actor path).** This removes the second full parse with no wire
     change, provided the Value leniency is reproduced.
  The full F-COORD-1/2 binary wire should wait until the helper's wire version is part of the compared build
  identity. The current gate hashes Sidecar.dll only.
- Could not verify by reading:
  - Real compression ratio, Value-parse and zlib timings (all estimates)
  - The commit rate per public room, which sets F-COORD-1's real VPS load
  - Whether `scripts/build-current.ps1` always relinks Sidecar.dll. That decides how likely a same-hash,
    different-helper package is.
  - The native `HandleHelperError` and CoordinationState asymmetry that F-SVC-5 mentions (not traced)
