# Adversary verdicts: game-session.md (F-SESSION-1 to 15)

Checked against sf4-nightly at b9ce0e31, read only. Nothing was built or run. All millisecond figures are still estimates.
Thread key: GT means the game thread (16.67 ms budget, `Steam_PostUpdate` -> `TickRuntime` -> `client.Step` -> `server->Step`,
src/sf4e/sf4e__UserApp.cxx:394-421). W means the HelperClient pipe worker thread. DW means `CheckpointDecodeWorker`.

### F-SESSION-1: CONFIRMED  (severity: keep P1, but the real cost is on the member side; see estimate)
- Decisive invariant/evidence:
  - Member path for a committed effect (GT, all within one tick unless parked):
    1. Envelope parse (IrohRoom.cxx:796).
    2. Payload DOM parse (IrohRoomPeers.cxx:288). There are two string copies around it: `get<std::string>` at :284 and the const copy into the Message at :317.
    3. `AuthorizedEffect`: a parse at IrohRoom.cxx:548, then `PayloadDigest` (a dump and a SHA, SessionRecovery.hxx:48-50) at :586, then a second dump at :611.
    4. `ClientAdapter::Poll` parses again at IrohRoomAdapters.cxx:118.
    5. `SessionClient::Step` parses again at sf4e__SessionClient.cxx:534.

    That is 4 DOM parses of the payload plus 1 envelope parse, 2 dumps and 1 SHA. The claim is accurate.
  - Leader path for a remote intent (GT): envelope parse (:796), payload parse (Peers:288), `IsRoomVerificationMessage` (RoomMessageQueue.hxx:25), `count_if` over the queue (n parses, :29), `IsRecoveryBatchableMessage` (2 parses, SessionServer.cxx:34-36), the verification filter (:175) and the main loop (:276). That is **7 parses + n** on the recovery path, not 6 + n, and 5 + n on the non-recovery `Poll` path. `SameQueuedRoomRetry` compares connection and payload first (IrohRoom.cxx:19), so it parses only on an exact duplicate, as the finding says.
  - Digest safety, which was the question asked:
    - The leader digests `Sha256(payload.dump())` (sf4e__SessionServer__Recovery.cxx:203-204) and splices the token textually (`CommittedEffectWire`, SessionRecovery.hxx:121-126).
    - The member re-canonicalizes: parse, erase `_commit`, then `dump()` (IrohRoom.cxx:584-586). nlohmann objects are std::map, so keys are sorted and the output is compact. Sender key order and whitespace therefore never reach the digest input.
    - Handing the parsed `json` onward after line 586 does not touch that input. `parse(dump(j)) == j` for nlohmann's own output (integers exact, doubles shortest round-trip, UTF-8 validated by the parser).
    - The digest must stay computed over `dump()` of the DOM. Any variant that hashes a substring of the received text (to skip the dump) would couple members to the leader's exact splice. Reject that variant explicitly.
- Adjustment:
  - (a) The tag needs a source on the local paths that queue strings without parsing: `ServerAdapter::Send` for connection 1 (Adapters.cxx:76), `ClientAdapter::Send` for the hosting branch (:151), and `PumpCommittedEffects` (IrohRoom.cxx:631/641).
  - (b) The tag must carry more than `type`. `IsRecoveryBatchableMessage` and `SameQueuedRoomRetry` need the room action's `kind` and `protocolVersion` (SessionServer.cxx:39-44, IrohRoom.cxx:21-25).
  - (c) Today `pendingAdmission_` is set at Adapters.cxx:142 *before* the hosting and peer checks. A hello that returns NotConnected and is retried (Client.cxx:481) has therefore already opened the fence. `BeginAdmission()` must be called before `Send`, not after a successful send, or the retry window differs.
  - (d) Keep `AuthorizedEffect` rewriting `payload` (the 16384 bound at Client.cxx:268), as the finding already says. Attach `body` only at drain, never in the 4 MB queues.
  - Thread: all GT. Saves GT CPU, and the decode worker is untouched.
- Estimate: the member side is about 3 DOM parses and 1 dump of about 13 KB per snapshot effect. At nlohmann on x86 MSVC that is roughly 0.6-1.5 ms per commit per member, which is plausible. The leader's O(queue) term matters only while a quorum round pauses the server, with up to 32 verification entries (RoomMessageQueue.hxx:30). Typical intents are under 1 KB, so the leader saving is about 0.1 ms per intent, small next to the member side. It is not a budget breach: the baseline's 16-member lifecycle p99 is 4.5 ms with these parses included (ROOM_PERF_BASELINE.md). It stays P1 because it is the largest per-commit GT cost on every member that needs no wire change.
- Duplicate of: no. This is the canonical finding; F-SESSION-2's "integer compares on the tag" and F-SESSION-14's parse count depend on it.

### F-SESSION-2: CONFIRMED  (severity: keep P1)
- Decisive invariant/evidence:
  - The digest at IrohRoom.cxx:586 runs before the gate at :591-605. A return of 0 makes `ClientAdapter::Poll` break with the head still queued (Adapters.cxx:108-109), so the head is re-parsed, dumped twice and hashed on every `SessionClient::Step`.
  - The parked time is real and ack-gated. The member's helper exports the checkpoint in `CHECKPOINT_WINDOW = 4` chunks of 16 KB (rust/sf4-net/src/recovery.rs:24-25). Credit advances only on the game's `checkpoint_ack` (service/checkpoints.rs:413-459), and the game acks per chunk from `Poll` (IrohRoom.cxx:417-422, 512).
  - The leader sends live effects right after its local commit (sf4e__SessionServer__Recovery.cxx:331-340). The effect therefore arrives as one message while the member's helper is still pushing about 29 chunks through about 7 credit windows. Then come decode on DW (11 ms) and import on the next `ObserveCoordination` tick.
  - Expect about 5-10 parked ticks. The finding's lower bound of 3 is too low.
  - `ActivateCommittedCheckpoint` parses every queued client message (IrohRoomCheckpoint.cxx:246-261).
- Adjustment:
  - The digest must still be computed and checked on **first sight**, as the finding's `digestChecked` cache does. A reorder-only variant (gate first, digest later) changes behavior. A corrupt or forged token with `term > coordination_.term` or `revision > effectsRevision_` would return 0 and park the head, stalling the client queue, instead of being dropped at once. Cache the decoded `EffectEnvelope` and the verdict on the entry, not the DOM. Re-parse once at delivery, or keep the DOM only for the head.
  - The `_terminal_replay` prefilter is safe as a prefilter: as a key it appears unescaped, while inside chat text its quotes are escaped. A string value equal to `_terminal_replay` only causes a harmless extra parse. Marking the entries at creation is cleaner.
  - Thread: GT.
- Estimate: about 0.3-0.6 ms per parked tick for a 13 KB head, times about 5-10 ticks, gives roughly 1.5-6 ms of GT per commit per member, spread over the transfer. It is plausible but unmeasured.
- Duplicate of: no. It is complementary to F-SESSION-1; the tag makes its cache trivial.

### F-SESSION-3: CONFIRMED  (severity: keep P2)
- Decisive invariant/evidence:
  - `Decode` builds `const std::string alphabet` on every call (CoordinationBytes.hxx:48). 64 characters exceeds MSVC's SSO, so that is one heap allocation per chunk. It then calls `find` once per input character (:52).
  - It runs on GT from `ConsumeCoordinationEvent`, through `Chunk` (CheckpointTransfer.hxx:56) and `CompleteReplayAcknowledgment` (:76).
  - `End` hashes the whole body on GT (:64).
  - The DW in ROOM_PERF_BASELINE.md item 2 covers only the final JSON decode (IrohRoomCheckpoint.cxx:57). NETPLAY_INVARIANTS.md:74-78 makes DW "the only room worker thread", so a faster GT decode is the right fix, not a second worker.
- Adjustment:
  - Per tick the cost is bounded by the 4-chunk credit window. A tick sees about 4-8 chunks, about 90-175K characters, which is about 0.5-1.5 ms today, not the whole commit at once. That is why it stays P2.
  - Test the table decoder against the current one for every rejection case (bad character, size, non-zero trailing bits, `%4==1`).
  - Thread: GT on both the member and the leader (`PumpCheckpoint`, IrohRoomCheckpoint.cxx:312-318).
- Duplicate of: no. Its "base64 leaves the pipe" tail is F-SESSION-5.

### F-SESSION-4: CONFIRMED  (severity: P3)
- Decisive invariant/evidence:
  - `coordination::Sha256` opens and closes the provider on every call and returns "" on failure (CoordinationBytes.hxx:19-33). `recovery_detail::Sha256` caches the provider, is already shared with DW and falls back to `Sha256Portable` (RoomDigest.cxx:14-29).
  - Call sites:
    - `ProposeCheckpointBytes` (IrohRoomCheckpoint.cxx:37-38), on the leader, GT;
    - `CheckpointReceiver::End` (CheckpointTransfer.hxx:64), on every member, GT;
    - ReplaySummaryCache.
- Adjustment:
  - Two behaviors change on CNG failure, and both should be listed. `checkpoint_digest_failed` becomes success, and `End`'s rejection (today `"" != digest`, which resets the transfer) becomes acceptance through the portable digest. Both are arguably fixes.
  - One provider open/close per commit is tens of microseconds, so P2 overstates it.
  - The off-Windows room host includes CoordinationBytes.hxx, so keep the `Sha256Portable` path.
- Duplicate of: no.

### F-SESSION-5: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence:
  - `header[4..5]` is not a "protocol byte". It is the u16 `wire::VERSION` (wire.rs:10, 48, 92). `read_frame` and `write_frame` are shared by `read_ipc` and by `read_control`, which is the QUIC control stream between helpers (wire.rs:32-37), and the same VERSION heads game UDP packets (wire.rs:118, 135).
  - The bootstrap also carries VERSION (ipc.rs:47) and the game requires `version == 1` (HelperClient.cxx:26).
  - The stdio host path copies frames with the same functions (stdio.rs:133-136).
  - The helper parses commands with `serde_json::from_slice` (service/entry.rs:39) and serializes events with `serde_json::to_vec` (:65).
- Same-build answer: yes, a raw-body frame can be introduced without runtime negotiation.
  - Windows: the game DLL and sf4-net.exe ship in one package, and the bootstrap version is checked on both sides. Bump an **IPC-only** version in the bootstrap and IPC frames, and a mismatched pair refuses to connect, failing closed.
  - Linux room host: there is no bootstrap, but README.md:67 and :94 build the host and the helper from one tree. An old helper that receives a raw frame fails its serde parse, which ends the reader loop and the IPC (entry.rs:39-44), so it also fails closed.
  - The proposed `status` capability is harmless but adds a second mode to test. Make the game accept both kinds on read only for the same-build tests.
- Adjustment:
  - Never bump the shared `wire::VERSION`. Fork `read_ipc`/`write_ipc` (or mark the body inside the payload) so QUIC control frames and UDP packets keep their header.
  - The byte saving is not a streaming win: credit is counted in raw bytes (4 × 16 KB), so the number of round trips per commit is unchanged, and a local pipe moves 600 KB in well under 1 ms.
  - The real gain is GT CPU: one unescape pass and two copies per message, plus base64. F-SESSION-3 gets most of the base64 part with no protocol change, so do it first.
  - Rust `Event` and `Command` need body-carrying variants, so the helper reviewer must sign off.
  - Thread: GT (event parse), W, and the helper.
- Duplicate of: no. F-SESSION-14's `send_many` and F-SESSION-3's tail depend on it.

### F-SESSION-6: CONFIRMED  (severity: keep P2)
- Decisive invariant/evidence:
  - The cited lines are wrong: HelperClient.cxx has 184 lines.
  - The real sites:
    - the loop at :144-181;
    - an idle `WaitForSingleObject(stop_, 2)` at :180;
    - a held-frame retry at :166;
    - `CreateEventW`/`CloseHandle` per `Transfer` at :59;
    - a `Send` that copies and does not wake the worker at :40-49;
    - `WriteFrame` building a vector at :90.
  - POSIX `Send` writes the wake pipe (HelperClientPosix.cxx:68-69) and `Run` waits with an infinite `poll` unless a frame is held (:161-167).
  - Constants and frame code are duplicated verbatim (:8-12 vs :17-21; :88-117 vs :106-131).
  - The pipe is byte mode (no `pipe_mode` override in rust/sf4-net/src), so two writes per frame are fine.
  - Only tests call `timeBeginPeriod`. src/sf4e/sf4e__UserApp.cxx:438-447 records the same uncertainty for GGPO ("1-15.6 ms ... depending on timer resolution").
- Adjustment:
  - With an overlapped header read kept pending, do not post the read while a frame is held back. That is the backpressure (as POSIX does by setting fd -1 at :166).
  - Use separate OVERLAPPED/event objects for read and write, `CancelIoEx` plus a wait on `Stop`, and keep the id-1 auth on the blocking path.
  - `Send(std::string)` by value also closes a wiping gap: today the caller's `dump()` temporary, which may hold a passphrase, is never wiped; only the `OutgoingMessage` copy is (HelperClient.hxx:24).
  - The latency effect on checkpoint transfer is per credit round trip. Each window pays up to two worker passes (the ack out, the chunks in). That is about 4 ms per window at 1 ms resolution, or up to about 31 ms at 15.6 ms resolution, when a window would otherwise cost about one 16.7 ms tick. At default resolution this could roughly double the transfer, and with it F-SESSION-2's parked ticks.
  - Thread: W only, plus one fewer copy on GT. No GT timing change.
- Duplicate of: no.

### F-SESSION-7: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence:
  - There are four pumps per tick:
    - `ObserveCoordination` (sf4e__NetplayRuntime.cxx:472);
    - `RoomRecoveryRuntime::Tick` (RoomRecoveryRuntime.hxx:13), inside it;
    - `ClientAdapter::Poll` (Adapters.cxx:103);
    - `ServerAdapter::Poll`/`PollRecoveryPrefix` (:24, :41).
  - Each pump takes 8 events (IrohRoom.cxx:794). The two server loops are copy-pasted.
  - The leader-loss freeze returns only from the current pump (IrohRoomPeers.cxx:216-224).
- Adjustment:
  - The redundant preambles cost microseconds, so this is structural, not overhead: P3.
  - It is not purely behavior-preserving.
    - Today later pumps catch events that W delivered while the tick was running, including the next checkpoint window acked earlier in the same tick. A single tick-start pump caps transfers at one credit window per tick and can lengthen F-SESSION-2's parked time. Measure with `CustomRoomGameTest --perf` before landing it.
    - The stricter freeze is a one-tick delay in the server's intent drain after leader loss. It is acceptable, but integration tests will see it.
    - `CloseRoom`'s `Poll(); Sleep(1)` loop (sf4e__NetplayRuntime.cxx:396) and the helper integration tests that use `RoomRecoveryRuntime` need the explicit `Pump`.
  - Thread: GT.
- Duplicate of: no.

### F-SESSION-8: CONFIRMED  (severity: P3)
- Decisive invariant/evidence:
  - `Begin` resets fields by hand in two indentation styles (IrohRoom.cxx:49-100).
  - Close-all-games repeats at :760, :786, :805 and :840.
  - Some state is process-scoped, not epoch state:
    - `abandonPending_`/`abandonEpoch_`, which must survive into the next `Begin` (:54-57);
    - `nextDecodeTicket_`, because in-flight DW results are matched by ticket (IrohRoomCheckpoint.cxx:48, 74). Resetting it could stage a previous room's decode;
    - `decoder_`.
- Adjustment: `latency_.Stop()` (:97) is a call, not a reset, so `epoch_ = EpochState{}` cannot replace it. It is a mechanical move with no runtime gain and real reset risk, so P3. Thread: none.
- Duplicate of: no. F-SESSION-9 overlaps with it.

### F-SESSION-9: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: the indentation counts are accurate (441 tab-indented lines, 340 space-indented). `if(allowed>0)` at Adapters.cxx:116 is dead after :109-114. The repository has no `.clang-format` and no `.git-blame-ignore-revs`.
- Adjustment: a whole-file reformat of IrohRoom.cxx and IrohRoomCheckpoint.cxx collides with every open or unpushed branch that touches them. Schedule it after those land, or limit it to the functions being edited. The `EffectVerdict` enum and removing the dead branch are safe. Thread: none.
- Duplicate of: partly F-SESSION-8 (the density mostly hides the repetition named there).

### F-SESSION-10: CONFIRMED for (a)-(c); (d) REJECTED  (severity: P3)
- Decisive invariant/evidence: retry cadences are counted in steps (+4/+8 at sf4e__SessionClient.cxx:505, 524, 527; the join request every 8). Messages are parsed once at :534; `payload` is used only for the 16384 bound (:268).
- Adjustment: drop (d). Converting step counts to milliseconds changes retry timing while loading, minimized or in background play, and the owner forbids behavior changes. (a)-(c) are mechanical. Thread: GT, unchanged.
- Duplicate of: no.

### F-SESSION-11: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence:
  - There are about 14 inline `recoveryPaused_` sites (RoomModel.cxx:25, 27, 33, 44, 50, 836, 856; RoomModelJson.cxx:16, 38, 41, 74, 79, 139, 152-153, 160, 308, 319-321).
  - There are three clock regimes today, not one:
    1. Live: instants.
    2. Passive replica: ages advanced with per-family caps. `AdvancePausedTimers` (RoomModel.cxx:118-124) is called only from sf4e__SessionServer__Rooms.cxx:215, 222, and caps at each timer's timeout, 1000 ms for chat and `MaximumIdleSeconds*1000` for activity.
    3. Actual control recovery: ages frozen, never advanced (RoomModel.hxx:706-708).
  - Permit timers form a fourth clock that is never paused (RoomModel.hxx:710-716).
- Player-observable timeouts: no change, if the type reproduces exactly:
  - `NoteActive` writing 0 while paused (:25, :27);
  - `ResumeRecovery`'s `rebasedNow = max(now, ages)` followed by `since = rebasedNow - age` (:133-143);
  - the caps;
  - `Checkpoint()` writing both `result_since` (instants, zeroed when paused) and `result_age` (RoomModelJson.cxx:74-75), plus `time` (:79).

  The ambitious variant ("a virtual clock that stops") would change the Ready timeout, the spectator start hold and the result-dispute deadline on passive replicas by the pause length, and would change checkpoint bytes that older members restore (Raft-replicated state). Reject it.
- Adjustment: model "advance capped" and "frozen" as two operations on the type, and exclude permits. It is legibility only, so P2 overstates it. Thread: GT, unchanged.
- Duplicate of: no. F-SESSION-12's timer split pairs with it.

### F-SESSION-12: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: the seams are as described (RoomModel.cxx 935 lines, RoomModel.hxx 994).
- Adjustment: none. The payoff is include fan-out only. Thread: none.
- Duplicate of: no.

### F-SESSION-13: CONFIRMED for (1); PLAUSIBLE for (2)  (severity: keep P2)
- Decisive invariant/evidence: `SnapshotFor` copies `snapshot_`, chat included (RoomModel.cxx:38), and the chat is then cleared for chat-delta clients (sf4e__SessionServer__Rooms.cxx:400-403). `json(snapshot)` is built per recipient (:410). It builds on baseline item 7 and is not per-table deltas, so it does not conflict with the prior art.
- Adjustment for (2): the splice must equal `json(snapshot).dump()` byte for byte. The leader digests that dump (Recovery.cxx:203-204), and a member re-dumps it before comparing (IrohRoom.cxx:585-586). Any divergence drops the snapshot effect on every member. Pin it on the 16-member fixture for three cases: `chatUnchanged` true, `chatUnchanged` false, and `--legacy-clients`. (1) is safe now.
- Thread: leader GT (`SessionServer::Step`). The 2-4 ms figure is unmeasured; the baseline host step is 6.3 ms in total.
- Duplicate of: no.

### F-SESSION-14: REJECTED  (severity: P3)
- Decisive invariant/evidence: the per-recipient `Send` is confirmed (SessionServer.cxx:180-183), but the cost is 2-10 KB/s on a local pipe, plus a few microseconds of `dump` per recipient on leader GT. The finding itself says not to change the format. `send_many` needs F-SESSION-5's frame kind and a new helper command.
- Adjustment: none. Revisit only if F-SESSION-5 lands. The parse count it cites is F-SESSION-1's.
- Duplicate of: F-SESSION-1 (parses) and F-SESSION-5 (pipe), which are canonical.

### F-SESSION-15: CONFIRMED  (severity: keep P3)
- Decisive invariant/evidence: `Game()` returns a copy (IrohRoom.cxx:172-175). It is used per link per tick in IrohMatchSession.cxx:464, 481, 492 and 514, which is the steady-match path. The "allocation-free per frame" rule in NETPLAY_INVARIANTS.md:53-54 is for diagnostics, but the intent applies.
- Adjustment:
  - `Game()` is virtual and overridden in src/tests/iroh_test_room.hxx:36. The new accessor must be virtual too, with the fake updated, or the match-session tests stop exercising it.
  - The existing `Games()` (IrohRoom.hxx:53) already returns a const reference; using it bypasses the fake, so do not.
  - Thread: GT per frame, and this is the only proposal on the rollback-hot path. Behavior is identical.
- Duplicate of: no.

## Batch verdict
- Counts by verdict:
  - CONFIRMED: 10 (F-1, 2, 3, 4, 6, 8, 9, 10 for (a)-(c), 12, 15).
  - PLAUSIBLE: 4 (F-5, 7, 11, 13 for (2)).
  - REJECTED: 1 (F-14), plus F-10 (d) and F-11's "stopped clock" variant.
- Severity changes:
  - F-4 from P2 to P3;
  - F-5 from P2 to P3;
  - F-7 from P2 to P3;
  - F-8 from P2 to P3;
  - F-10 from P2 to P3;
  - F-11 from P2 to P3.
- Estimate corrections:
  - The F-1 leader count is 7 + n on the recovery path.
  - F-2 parks for about 5-10 ticks, because transfer is ack-gated by a 4-chunk window.
  - F-6's cited lines do not exist (the file has 184 lines).
- Do first:
  1. F-SESSION-2: a digest-once cache on the queue entry. Small, local, the largest GT ms per commit per member, and no wire change. It must still check the digest on first sight.
  2. F-SESSION-3: a table-driven base64 decode. A pure function and testable against the old one. It removes per-chunk allocations and about 600K lookups per commit from GT.
  3. F-SESSION-1: tag at the edge and hand the DOM to SessionClient. It is the biggest structural win, and it subsumes F-2's gate and F-14's parse count. The digest stays over `dump()` of the DOM.

  F-SESSION-6 is a close fourth: worker-only, and it may matter a lot if the process runs at 15.6 ms resolution.
- Could not verify by reading:
  - The process timer resolution inside SSFIV.exe. No idalib session was open, and only tests call `timeBeginPeriod`. This decides whether F-6's waits are about 2 ms or about 15.6 ms.
  - Every millisecond figure (no `RoomHostBench` or `--perf` runs).
  - The actual number of parked ticks per commit.
  - F-5's claim that the QUIC checkpoint bytes between helpers stay the same (I did not read the helper's replication path).
  - Rust-side `Event`/`Command` changes for F-5.
  - Whether the bridge or SDK partners consume `read_control` frames, which matters only if someone bumped the shared VERSION.
