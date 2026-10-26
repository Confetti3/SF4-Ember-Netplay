# Findings: rust/sf4-net/src/service (helper actor), non-test modules

Reviewed at nightly head b9ce0e31: mod.rs, controls.rs, members.rs, refresh.rs, checkpoints.rs, departure.rs,
games.rs, seats.rs, server_owned.rs, public.rs (to line 120), protocol.rs, events.rs, entry.rs, stall.rs,
network.rs (to line 140), probes.rs (to line 340), latency.rs, latency/authority.rs, short_links.rs (skimmed),
join.rs (head). Cross-checked native consumers in src/session/IrohRoom.cxx and IrohRoomPeers.cxx.

### F-SVC-1: Per-room state is ~45 loose Actor fields reset by hand in two near-identical 40-line lists
- File: rust/sf4-net/src/service/mod.rs:624  (Actor, 60 fields)
- File: rust/sf4-net/src/service/mod.rs:861  (clear_room, 58 lines of field resets)
- File: rust/sf4-net/src/service/departure.rs:582  (enter_departure_grace, 41 lines of the same resets)
- File: rust/sf4-net/src/service/entry.rs:95  (60-line struct literal; tests.rs:270 repeats it)
- Category: structural
- Severity: P1
- Claim: Everything that belongs to "the current room" (admissions, admission_order, pending_admissions,
  applied_admission_members, the checkpoint transfer fields, retirement sets, the pending_* operation keys,
  probe maps, parked controls, games, join_* counters, public, short, seats, refresh_roster, markers) is a
  separate top-level Actor field. Leaving a room is therefore two hand-maintained reset lists. clear_room
  and enter_departure_grace share 37 identical lines; they differ only in what grace silently keeps:
  room, recovery, epoch, short, refresh_roster (an Arc of the whole ~460 KB committed checkpoint, per the
  comment at mod.rs:872, held for the 30 s grace), seats, join_settled/join_control_losses/join_first_loss,
  departure_failed, closed_generation, last_exported_revision, coordination_writable, latency (cleared
  separately by leave_command). Nothing says which of those survivals are intended. Every new per-room
  field must be added to three or four places (two resets, entry.rs, tests.rs) or it leaks across rooms.
  The room lifecycle itself is encoded as a combination of `room: Option`, `opening: bool`,
  `retirement_started: Option<Instant>` and `departure_failed: bool`, which tick_period, begin, accept
  gating and emit_coordination_state each re-derive. For example (observed, not verified at runtime):
  during grace `recovery` and `room` stay set and `last_exported_revision` is not reset, and only
  emit_coordination_state checks `retirement_started` (refresh.rs:107). pump_committed_checkpoint
  (checkpoints.rs:265) has no grace check, so for the full 30 s it keeps reading the store every tick and
  would export a commit landing in the window (the leader committing this member's removal is one) to a
  native side that already got room_closed; with no ack that transfer then times out and retries.
  That reads as an accidental survivor list rather than a designed one.
- Proposed change: Move every per-room field into `struct RoomState` with `#[derive(Default)]` (or an
  explicit `new()`), held as `self.room_state`. Model the lifecycle as
  `enum Phase { Idle, Opening, Open, Grace { recovery, room, started } }`. clear_room becomes "stop
  recovery; room_state = RoomState::default(); phase = Idle"; enter_departure_grace becomes "move recovery
  and room into Phase::Grace; room_state = RoomState::default()". The grace survivors become an explicit,
  reviewable list in one variant. entry.rs and tests.rs construct `RoomState::default()`.
- Behavior preserved: All transitions; what grace keeps can be reproduced exactly by putting those fields
  in Grace or leaving them outside RoomState.
- Behavior at risk: Any field whose survival through grace is accidental but relied on (join_* counters,
  closed_generation, last_exported_revision). Decide each explicitly; tests in tests.rs cover leave/rejoin.
- Estimated saving: ~120 lines deleted (two reset lists, most of two struct literals), 3 lifecycle flags
  collapse into one enum, the 460 KB refresh_roster Arc is released at grace start if not kept on purpose.
- Confidence: high

### F-SVC-2: mod.rs (1540 lines) mixes wire types, task keys, feature state, the command handler and the loop
- File: rust/sf4-net/src/service/mod.rs:183  (Completion and the *Key/*Result structs, 183-350)
- File: rust/sf4-net/src/service/mod.rs:947  (command(), 275 lines)
- File: rust/sf4-net/src/service/mod.rs:1371  (run loop and statistics arm)
- Category: size
- Severity: P1
- Claim: The feature modules (games.rs, checkpoints.rs, probes.rs, controls.rs, members.rs) hold the
  behavior while mod.rs still holds their state types and the biggest command arms. Clear seams:
  (a) 110-138 native_json_depth_ok and MAX_NATIVE_JSON_DEPTH: a native-JSON protocol concern -> protocol.rs.
  (b) 183-350 Completion enum plus CheckpointProposalKey, CoordinationRefreshKey/Refresh,
      MembershipOperationKey/Result, AdmissionOperationKey/Result, DeferredAdmission, ControlBinding,
      ProbeAuthorizationKey, ProbeReservationKey, ProbeAuthorization -> a completion.rs, or each key next
      to the module that spawns and settles it (members.rs, refresh.rs, probes.rs, checkpoints.rs).
  (c) 352-405 and 497-512 GameSlot, GameDiagnostics -> games.rs; OutgoingCheckpoint,
      PendingCheckpointCommitted -> checkpoints.rs; ParkedControl -> controls.rs; PendingProbeInvalidation,
      ProbePermission, OwnProbe, ProbeCompletion -> probes.rs.
  (d) 444-482 CoordinationControl is the peer-to-peer wire enum -> protocol.rs beside NativeControlMessage.
  (e) 554-622 primary_members and applied_member_incarnations -> seats.rs/members.rs (see F-SVC-4).
  (f) 1006-1066 Command::Send -> controls.rs `send_native`; 1067-1076 CloseControl -> controls.rs;
      1077-1211 PrepareGame/EndMatch/EndPeer -> games.rs `prepare_game`/`end_match`/`end_peer`.
  (g) 1326-1369 completed_hosted -> public.rs (where spawn_host lives) or a host.rs.
  (h) 1497-1536 the statistics arm body -> one `Actor::second()` in stall.rs or a new periodic.rs, so the
      select! loop reads as dispatch only.
- Proposed change: Do the moves above; mod.rs keeps constants, Actor/RoomState, emit helpers and run().
- Behavior preserved: Pure moves.
- Behavior at risk: none identified.
- Estimated saving: mod.rs to roughly 550-650 lines; each feature's state and behavior in one file.
- Confidence: high

### F-SVC-3: Two-tier command dispatch with a production-dead Leave arm, and a completion pre-match with unreachable!() arms
- File: rust/sf4-net/src/service/mod.rs:997  (Command::Leave in command(): `let _ = abandon;`, clear_room, RoomClosed)
- File: rust/sf4-net/src/service/mod.rs:1212  (async_command_not_dispatched arm)
- File: rust/sf4-net/src/service/mod.rs:1398  (run() intercepts Checkpoint*, ProbeRequest, Leave)
- File: rust/sf4-net/src/service/mod.rs:1224  (completed(): first match rewrites three variants, second match has unreachable!())
- Category: judo
- Severity: P2
- Claim: command() is sync, so run() pre-routes the async commands and command() carries an error arm for
  them. The Leave arm in command() can never run in production (run() sends every Leave to leave_command),
  but tests.rs:3972 and 3991 call actor.command(Leave) directly, so tests exercise an immediate
  clear_room leave that production never takes. completed() first rewrites GuestControl, MemberControl and
  Reconnect into Control plus an Option<Invite>, then matches again with unreachable!() for the rewritten
  variants.
- Proposed change: Make `async fn command(&mut self, request, commands, failed_ipc)` the single dispatcher
  (Leave calls leave_command; checkpoint and probe commands map their Err to the same error codes). Delete
  the dead Leave arm and the async_command_not_dispatched arm. In completed(), give each of the three
  variants its own handler that ends in `self.completed_control(epoch, result, invite)`, and drop the
  pre-match and both unreachable!() arms. Name the positional Completion tuples with 4-5 fields
  (ShortResolved, GameCandidate, Game) as struct variants.
- Behavior preserved: Production dispatch is unchanged.
- Behavior at risk: The two tests using the sync Leave arm must move to the real leave path (they then
  test what ships).
- Estimated saving: ~35 lines, one dispatcher instead of two, two unreachable!() removed.
- Confidence: high

### F-SVC-4: Every committed checkpoint (~460 KB) is parsed into a full serde_json::Value tree twice per member, and twice more per probe
- File: rust/sf4-net/src/service/seats.rs:180  (observe_commit: from_slice::<Value>, cached per commit)
- File: rust/sf4-net/src/coordination/latency_roster.rs:74  (committed_latency_roster: Value parse, separate cache keyed (revision, term, digest) in service/latency/authority.rs:205)
- File: rust/sf4-net/src/coordination.rs:640  (committed_probe_pair_bound: Value parse, reached from probes.rs:189 and again from probes.rs:261 per probe)
- File: rust/sf4-net/src/service/mod.rs:554  (primary_members walks the Value)
- Category: overhead
- Severity: P1
- Claim: The helper needs only checkpoint.members[*].{frozen,incarnation,member,data.authenticatedEndpoint}
  and checkpoint.room.snapshot.{members,tables,tournament}. Yet each commit is materialized as a full Value
  tree twice (seats/native roster and latency roster), with two independent caches keyed differently. Per
  docs/perf/ROOM_PERF_BASELINE.md the envelope is ~460 KB, so that is ~920 KB of tree-building JSON per
  commit on every member, on runtime workers shared with gameplay bridges. A probe check parses it twice more.
- Proposed change: One `CommitView` derived once per commit (cache key: Committed::same_commit), built with a
  typed borrowed deserializer: `#[derive(Deserialize)] struct Envelope<'a> { #[serde(borrow)] checkpoint:
  Option<CheckpointView<'a>> }` where the large irrelevant subtrees are skipped (serde ignores unknown fields
  without allocating) and only the small arrays it reads are kept, as `Value` where the current code's
  lenient type handling must be reproduced exactly. CommitView exposes native_roster(allow_empty), seats(),
  latency_roster() and probe_pair(...). observe_commit, latency Observer::roster and probe_pair_bound all
  read it. Payload type: Raft-committed SessionProposal JSON; size ~460 KB; interval: every room commit
  (every Ready, chat, result, ack at any table); fan-out: every member's helper; transport: in-process.
- Behavior preserved: Same accept/reject outcome per commit if the view keeps Value for the member rows and
  the tournament object (small) and only skips the rest.
- Behavior at risk: A strictly typed field where the code now tolerates a type mismatch would turn "None"
  into a parse error; keep Option/Value at those points. No wire change.
- Estimated saving: rough, from serde_json throughput (Value tree ~200-400 MB/s vs skip-parse ~1 GB/s):
  ~2-4 ms CPU and ~1-2 MB transient allocation per commit per member, plus ~2 parses per probe. Removes one
  of two parse caches.
- Confidence: medium (size from the perf baseline doc; timings are estimates, not measured)
- Note: overlaps the coordination area (coordination/latency_roster.rs:74, coordination.rs:640); merge with
  any finding there rather than counting it twice.

### F-SVC-5: ControlRebound is re-emitted every second while unchanged, and its marker is a dead store
- File: rust/sf4-net/src/service/refresh.rs:398  (rebound_marker built and stored; never read)
- File: rust/sf4-net/src/service/mod.rs:692  (last_control_rebound: only ever assigned)
- File: rust/sf4-net/src/service/mod.rs:1500  (emit_coordination_state every statistics tick)
- File: src/session/IrohRoom.cxx:445  (native handler re-parses, rebuilds std::map/std::set, calls ApplyControlRebound)
- Category: overhead
- Severity: P2
- Claim: last_control_rebound is assigned at refresh.rs:418 and reset to None at 9 sites, but nothing
  compares against it. The comment at refresh.rs:403 says the resend is deliberate, and it is load-bearing
  today: the native side clears coordination_.rebound on a term change (IrohRoom.cxx:429) and on any
  helper error scoped to the leader (IrohRoom.cxx:722), and only the next control_rebound sets it back.
  Note the asymmetry: CoordinationState is marker-cached, yet the same native error path also clears
  writable, which only a changed CoordinationState restores (worth a separate look by the native reviewer).
  Payload: Event::ControlRebound {leader, members: Vec<String>, member_incarnations: BTreeMap<String,u64>};
  size ~2.5-3 KB in a 16-member room (64-char endpoint ids); interval 1 s for the room's life; fan-out 1 per
  helper (every member PC); transport: helper IPC pipe. Each second also builds roughly a hundred Strings
  in the helper (connected set 16, member_incarnations keys 16, members clone 16, marker clone ~33, plus
  the per-admission and per-control to_string compares counted in F-SVC-11).
- Proposed change: Step 1 (no risk): delete last_control_rebound, its 9 resets and the per-second marker
  clone. Step 2: send on change, using a marker that is invalidated (a) whenever CoordinationState is
  emitted, (b) whenever the actor emits an Event::Error whose peer is None or the current leader (route all
  Error emission through one helper so this is one line), and (c) by a keepalive every 10 s. Apply the
  same invalidation to last_coordination_state, which closes the asymmetry above.
- Behavior preserved: Native receives a fresh snapshot after every change, every term change and every
  leader-scoped error, which are the only things that clear its rebound flag.
- Behavior at risk: A native reset path not listed above would wait up to the keepalive (10 s) instead of
  1 s. Same-build IPC, so no old-client concern.
- Estimated saving: ~2.5 KB/s of pipe JSON per helper and one native JSON parse plus map/set rebuild per
  second; roughly 90% of it in steady state.
- Confidence: medium

### F-SVC-6: Membership publication re-serializes the full admission roster per peer and repeats it every refresh while any retirement is pending
- File: rust/sf4-net/src/service/controls.rs:308  (send_membership_control: build, filter, serialize per peer)
- File: rust/sf4-net/src/service/controls.rs:350  (pump_membership_publications loops peers)
- File: rust/sf4-net/src/service/refresh.rs:239  (leader queues a publication to all controls on each refresh while pending_retired is non-empty)
- File: rust/sf4-net/src/service/controls.rs:537  (receiver: provenance read, forced CoordinationState + ControlRebound, admission operation)
- Category: overhead
- Severity: P2
- Claim: Payload: CoordinationControl::Membership {admissions: Vec<Admission>, retired: Vec<u64>}; each
  Admission is ~400-500 B of JSON (room as a 16-number array, two 64-char ids, an EndpointAddr with relay
  URL and direct addresses), so ~6-8 KB for 16 members. The payload is identical for every peer
  (retired_before_retry is always empty from the pump, membership_view is peer-independent), yet it is
  rebuilt and serialized once per peer. Interval: every admission completion, every leadership gain, and
  every 1 s coordination refresh (plus each completion-triggered refresh) for as long as any retirement is
  pending, which lasts a membership operation (up to the 12 s timeout, retried while it fails). Fan-out:
  15 controls. Transport: QUIC control streams. Each receipt makes every follower re-read applied
  membership, force-emit CoordinationState and ControlRebound over IPC, and queue an admission operation.
- Proposed change: (1) Serialize once per publication and clone the bytes per peer (no behavior change).
  (2) Keep per (peer, ControlWorker id) a digest of the last Membership payload that peer's queue
  accepted, and skip a republish with the same digest; a new control id (reconnect) always gets it.
  (3) Change refresh.rs:239 to "retirement set changed since the last publication" instead of "non-empty".
- Behavior preserved: Every control still receives every distinct roster at least once, and every new
  control receives the current one.
- Behavior at risk: A follower whose own admission operation failed currently gets an implicit retry from
  the next identical publication; with (2) it would wait for the next roster change. Keep a slow forced
  republish (for example every 10 s) to preserve that. No wire change, so older helpers are unaffected.
- Estimated saving: (1) 14 of 15 serializations per publication. (2)+(3) about 100-120 KB/s of QUIC
  control traffic from the leader, and 15 follower-side refresh+IPC bursts per second, during each pending
  retirement window.
- Confidence: medium

### F-SVC-7: The committed-checkpoint pump takes the Raft store mutex on every actor tick (500/s during a match)
- File: rust/sf4-net/src/service/checkpoints.rs:271  (`recovery.committed().await` each tick)
- File: rust/sf4-net/src/coordination.rs:492  (committed() locks the store's tokio Mutex)
- File: rust/sf4-net/src/service/refresh.rs:95  (install_recovery already subscribes committed_changes and only forwards it to `wake`)
- Category: overhead
- Severity: P2
- Claim: pump_committed_checkpoint runs at the end of every tick (FAST_TICK 2 ms while any game slot exists,
  25 ms with a room) and locks the Raft store to compare a revision, though a watch channel of that very
  revision exists and its forwarding task already wakes the actor. latency_tick already uses the lock-free
  has_changed() pattern for its own watch.
- Proposed change: Have the forwarding task in install_recovery store the latest revision into an
  `Arc<AtomicU64>` (or keep the watch::Receiver on the actor and read `borrow()`), and return early from
  pump_committed_checkpoint while `latest <= last_exported_revision`. The timeout path that lowers
  last_exported_revision keeps working because the comparison is against the cached latest revision.
- Behavior preserved: Same export order and retry semantics.
- Behavior at risk: none identified, provided the cached revision is updated before `wake` is notified.
- Estimated saving: ~500 async mutex acquisitions per second per helper during matches (40/s otherwise), and
  their contention with Raft apply on the runtime workers that also run gameplay bridges.
- Confidence: high

### F-SVC-8: Native messages are copied and parsed several times per hop, and the IPC escapes JSON inside JSON
- File: rust/sf4-net/src/service/protocol.rs:567  (serialize clones the payload, up to 64 KB, only to validate it as RawValue)
- File: rust/sf4-net/src/service/mod.rs:1031  (kind: "native_control".into() allocated per send)
- File: rust/sf4-net/src/service/controls.rs:739  (receive: RawValue parse, then to_owned copy)
- File: src/session/IrohRoomPeers.cxx:284  (native: event parsed, payload string copied, parsed again)
- Category: overhead
- Severity: P2
- Claim: NativeControlMessage fixed escape-doubling on QUIC, but the IPC hop on both sides still carries
  `payload` as a JSON string: native dump()s the message and embeds it as a string (escaping every quote),
  the helper unescapes it, clones it, re-validates it and copies it into the frame; the receiving helper
  validates and copies it again and re-escapes it into Event::Message, and native parses twice. Payload: room
  live sends and server intents; up to 64 KB each; per room commit the host sends ~164 KB in total
  (perf baseline); fan-out per member; transport: IPC pipe and QUIC.
- Proposed change: Phase 1 (Rust only, no wire change): serialize with
  `serde_json::from_str::<&RawValue>(&self.payload)` (borrow, no clone); make `kind` a `&'static str`.
  Phase 2 (IPC only): carry `payload` as raw JSON in Command::Send and Event::Message, accepting a string
  too during transition; native stops dump-then-parse. The QUIC frame format is unchanged.
- Behavior preserved: Same bytes on QUIC; same validation (the depth checks at controls.rs:736 and 745 stay;
  the inner one is still the only bound for a legacy string-encoded payload).
- Behavior at risk: Phase 2 changes the helper/DLL IPC contract; both ship in the same package, so no old
  client mixes them, but the native reader should accept both forms for one release if a mixed install is
  possible.
- Estimated saving: Phase 1: one payload-sized allocation and memcpy per send. Phase 2: ~10-20% of payload
  bytes in escaping each way on the pipe and one JSON parse per message on each side.
- Confidence: high (phase 1), medium (phase 2 sizing)

### F-SVC-9: Admission equality is computed by serializing admissions to JSON bytes
- File: rust/sf4-net/src/service/members.rs:334  (fingerprint = serde_json::to_vec(&admissions))
- File: rust/sf4-net/src/service/mod.rs:290  (AdmissionOperationKey.fingerprint: Vec<u8>)
- File: rust/sf4-net/src/recovery.rs:709  (Admission derives no PartialEq)
- Category: judo
- Severity: P3
- Claim: queue_admission_operation dedupes operations by comparing serialized bytes, so every Admission
  frame, every refresh-driven retry (refresh.rs:281) and every Membership receipt serializes the batch.
  EndpointAddr in iroh-base 1.1.0 derives PartialEq/Eq, so Admission can derive PartialEq.
- Proposed change: `#[derive(PartialEq, Eq)]` on Admission; replace `fingerprint: Vec<u8>` with comparison
  of `admissions` in DeferredAdmission and an `Arc<[Admission]>` (or the vector) in AdmissionOperationKey.
- Behavior preserved: Equality of the same fields; serialization was deterministic, so outcomes match.
- Behavior at risk: none identified.
- Estimated saving: one serialization (~0.5-8 KB) per admission batch queued; a concept removed.
- Confidence: high

### F-SVC-10: Copy-pasted control-frame sends and marker resets
- File: rust/sf4-net/src/service/controls.rs:26  (also controls.rs:122, controls.rs:329, mod.rs:1043, probes.rs:53, server_owned.rs:146)
- File: rust/sf4-net/src/service/members.rs:513  (the `last_coordination_state = None; last_control_rebound = None; emit_coordination_state()` triad at 7 sites: controls.rs:445/607/982, departure.rs:368, members.rs:513/583, refresh.rs:488; plus partial resets at mod.rs:901, departure.rs:610, checkpoints.rs:588)
- File: rust/sf4-net/src/service/refresh.rs:236  (marker read positionally: marker.3, marker.0; also short_links.rs:338)
- Category: spaghetti
- Severity: P2
- Claim: Six sites hand-roll "take next_transport_message, bump it, build ControlFrame, try_send", with
  inconsistent semantics (send_membership_control bumps only on success, the others burn the id on
  failure). Seven sites force a coordination republish with the same three lines. The CoordinationState
  marker is a 6-tuple read as `.3` (leader_local) and `.0` (term) in two modules.
- Proposed change: `fn send_coordination(&mut self, peer, &CoordinationControl) -> Result<(), QueueError>`
  (serializes, numbers, sends) and `fn send_frame(&mut self, peer, Vec<u8>)`; `async fn
  republish_coordination_state(&mut self)`; a named `CoordinationMarker` struct.
- Behavior preserved: Ids stay monotonic; the native side never sees transport ids.
- Behavior at risk: none identified (id gaps already occur).
- Estimated saving: ~60 lines; one id policy.
- Confidence: high

### F-SVC-11: Endpoint identities are compared as strings in the per-second refresh
- File: rust/sf4-net/src/service/refresh.rs:294  (state.leader holds a coordination endpoint string, then is overwritten with the primary endpoint string)
- File: rust/sf4-net/src/service/refresh.rs:298  (`admission.coordination_endpoint.to_string() == leader_coord` per admission)
- File: rust/sf4-net/src/service/refresh.rs:367  (connected: BTreeSet<String>; also refresh.rs:388)
- File: rust/sf4-net/src/service/protocol.rs:236  (ControlRebound leader/members/member_incarnations as String)
- Category: boundary
- Severity: P2
- Claim: AuthorityState.leader carries two different identities in one String field within one function,
  and every second the refresh allocates a String per admission and per control to compare ids. iroh-base
  1.1.0 serializes EndpointId as its Display string for human-readable serializers (key.rs:78), so typed
  fields keep the JSON identical (Event::Connected already does this).
- Proposed change: Resolve the leader as `Option<EndpointId>` (coordination) -> `Option<&Admission>` ->
  primary EndpointId; type ControlRebound/CoordinationState leader as Option<EndpointId> serialized as ""
  when absent (a small serialize_with), members as Vec<EndpointId>, member_incarnations as
  BTreeMap<EndpointId, u64>. applied_member_incarnations returns EndpointId keys.
- Behavior preserved: Identical IPC JSON.
- Behavior at risk: The empty-string leader must stay "" on the wire.
- Estimated saving: together with F-SVC-5 step 1, roughly a hundred String allocations and ~30 string
  compares per second per helper (to_string per admission at refresh.rs:298 and per control at :388 are
  ~31 of them); one overloaded field removed.
- Confidence: medium

### F-SVC-12: The once-a-second coordination refresh does ~20 awaited store reads and rebuilds everything
- File: rust/sf4-net/src/service/refresh.rs:137  (task: applied_membership_provenance, `rpc.is_retired` once per admission, state, committed, joint)
- File: rust/sf4-net/src/service/refresh.rs:422  (completion locks committed() again)
- Category: overhead
- Severity: P3
- Claim: Every second, with nothing changed, the refresh task performs up to ~20 lock acquisitions (one per
  admission for is_retired) and the completion recomputes retirements, the native roster lookup, the
  pending-admission retry filter and the ControlRebound inputs. It is the driver behind F-SVC-5 and
  F-SVC-6.
- Proposed change: Batch is_retired into one call returning the retired set; skip apply_coordination_refresh
  work when (term, leader, committed revision, applied membership, controls set) is unchanged, except the
  parts F-SVC-5 keeps on a keepalive.
- Behavior preserved: Outputs identical when inputs are identical.
- Behavior at risk: Time-based work reached from the refresh (unwritable_leader_since grace, pending
  admission retries) must still run each second; keep those branches unconditional.
- Estimated saving: ~15-20 awaited mutex acquisitions per second per helper, plus the recomputation.
- Confidence: medium

### F-SVC-13: Small duplications and stringly-typed error classification
- File: rust/sf4-net/src/service/mod.rs:1528  (Statistics event built inline; same 10-field construction at games.rs:336)
- File: rust/sf4-net/src/service/games.rs:166  (two ProbeCompletion literals differing only in `connection`)
- File: rust/sf4-net/src/service/probes.rs:275  (error class recovered by matching io::Error message text)
- File: rust/sf4-net/src/service/network.rs:126  (`error.to_string() == "relay_unavailable"`)
- File: rust/sf4-net/src/service/departure.rs:503  (Event::Error literal repeated 5 times in leave_command)
- Category: spaghetti
- Severity: P3
- Claim: `failed(&'static str)` turns codes into io::Error text, and two places parse the text back into a
  code; probe failure reasons are magic numbers 1-11 at eight call sites.
- Proposed change: `fn statistics_event(epoch, peer, &GameSlot) -> Option<Event>`;
  `connection: serve_probe(..).await.ok()`; a `ProbeFailure` repr(u8) enum; a small typed error (or
  io::Error with a custom payload) for the codes that are classified later; `fn error_event(request_id,
  epoch, code) -> Event` used by leave_command and Actor::error*.
- Behavior preserved: Same event JSON and codes.
- Behavior at risk: none identified.
- Estimated saving: ~70 lines; two string-matching boundaries removed.
- Confidence: high

### F-SVC-14: Checkpoint export base64-encodes ~460 KB per commit onto the IPC pipe
- File: rust/sf4-net/src/service/checkpoints.rs:497  (URL_SAFE_NO_PAD.encode per 16 KB chunk into a JSON event)
- File: rust/sf4-net/src/service/checkpoints.rs:300  (full to_vec copy of the committed checkpoint per commit)
- Category: overhead
- Severity: P3
- Claim: Payload: Event::CheckpointChunk with 16 KB of checkpoint as base64 in a JSON string, window 4; size
  ~460 KB per commit becomes ~613 KB of base64 plus per-chunk envelopes; interval: every commit; fan-out:
  every member; transport: IPC pipe. Both directions: the proposal path carries the same inflation into
  the leader's helper (Command::CheckpointChunk { data: String }, decoded in IncomingTransfer::push and at
  checkpoints.rs:138 on retry), so the host's helper takes ~613 KB in and ships ~613 KB out per commit.
  The helper also copies the committed bytes once per export though Committed is already behind an Arc.
- Proposed change: Hold `Arc<Committed>` in the outgoing transfer instead of a Vec copy (needs
  CheckpointTransfer to accept shared bytes). For the encoding, the IPC frame payload is already Vec<u8>:
  a binary chunk frame (or raw bytes after a small JSON header) would remove the 33% inflation and the
  encode/decode on both sides. Do not reopen per-table deltas (ROOM_PERF_BASELINE explains why not).
- Behavior preserved: Same bytes reach the native receiver; same credit window.
- Behavior at risk: IPC contract change (same-build helper and DLL); the native checkpointReceiver_ must
  take the new frame kind.
- Estimated saving: ~150 KB of pipe bytes, one 460 KB copy, and base64 encode+decode per commit per member.
- Confidence: medium

## Area summary
- Health: behavior is carefully reasoned and heavily commented, but the actor's state model has not kept
  up: ~45 per-room fields live flat on Actor and are reset by two hand-maintained lists (F-SVC-1), and
  mod.rs still owns most feature types and the largest command arms (F-SVC-2).
- Biggest judo move: a RoomState struct plus a Phase enum (F-SVC-1), which deletes both reset lists, three
  lifecycle flags and the struct-literal duplication; second, a single async command dispatcher (F-SVC-3).
- Biggest overhead item: the double full-Value parse of every ~460 KB committed checkpoint on every member
  (F-SVC-4). Biggest recurring sends: ControlRebound every second unchanged with a dead marker (F-SVC-5,
  ~2.5 KB/s per helper on the pipe) and full-roster Membership republication at 1 Hz x 15 peers during
  retirement windows (F-SVC-6, ~100 KB/s QUIC while it lasts).
- Per-tick: the 500 Hz Raft-mutex read in pump_committed_checkpoint (F-SVC-7) is the only per-tick item
  worth changing; the rest of the tick is cheap map scans.
- The latency board (MemberLatency) is already send-on-change and droppable, its roster parse is cached,
  and its 15 s LEASE is a native renewal, not a payload; only its duplicate checkpoint parse matters (folded
  into F-SVC-4). NetworkReport is send-on-change. Nothing in this area sends a large JSON every 15 s.
- Not assessed in depth: probes.rs 340-754, public.rs 120-227, short_links.rs bodies, latency/opening.rs and
  latency/endpoint.rs (signatures only), and the coordination/recovery crates behind the awaited calls. No
  timings were measured; CPU estimates are throughput rules of thumb.
