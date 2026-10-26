# Game-side session / netplay / platform pipe review (F-SESSION)

Scope: src/session/*, src/netplay/* (SessionController, TournamentPlay, MatchResultOutbox skimmed), src/platform/HelperClient*.cxx,
src/common pipe-adjacent files (CoordinationBytes, SkillBadgeJson, ConfirmedCheckpoint, NetworkRoute). Head b9ce0e31, read-only, nothing run.
Millisecond figures are estimates (nlohmann DOM parse assumed ~30-50 MB/s, dump ~100-200 MB/s on the x86 MSVC build); confirm with
`RoomHostBench` and `CustomRoomGameTest --perf` before quoting them.

### F-SESSION-1: A room message is a bare string, so every hop parses it again to find out what the first hop already knew
- File: src/session/SessionTransport.hxx:21
- File: src/session/IrohRoomPeers.cxx:282
- File: src/session/RoomMessageQueue.hxx:12
- File: src/session/IrohRoom.cxx:18
- File: src/session/IrohRoom.cxx:545
- File: src/session/IrohRoomAdapters.cxx:102
- File: src/session/IrohRoomAdapters.cxx:140
- File: src/session/sf4e__SessionServer.cxx:30
- File: src/session/sf4e__SessionServer.cxx:175
- File: src/session/sf4e__SessionServer.cxx:296
- File: src/session/sf4e__SessionClient.cxx:531
- Category: judo
- Severity: P1
- Claim: `session::Message` carries only `payload` as text. `ReceiveControlMessage` parses it (IrohRoomPeers.cxx:288), takes its type and `_commit` flag, then throws the parse away. After that, each layer runs `json::parse` again to recover the same facts:
  - Leader's game thread, client intent bound for the server queue: helper event parse (IrohRoom.cxx:796), ReceiveControlMessage parse, then `QueueRoomMessage`. That runs `IsRoomVerificationMessage(message)` (one parse) and `count_if(..., IsRoomVerificationMessage)` (one parse for each queued entry, up to 64) on every push. Under pressure it also runs `find_if` (more parses). `SameQueuedRoomRetry` adds a parse on an exact duplicate. `SessionServer::Step` then parses again in `IsRecoveryBatchableMessage` (up to 2), again in the verification filter (line 175) and again in the main loop (line 296). That is 6 parses plus n per enqueue for one intent.
  - Member's game thread, committed effect: helper event parse, ReceiveControlMessage parse, then `AuthorizedEffect` (parse, erase `_commit`, dump for the digest, SHA, dump again into `message.payload`). Then `ClientAdapter::Poll` parses again only to see `join_rej`/`hello_resp` (Adapters.cxx:118), and `SessionClient::Step` parses again (Client.cxx:534). That is 4 full parses, 2 full dumps and 1 SHA.
  - Outgoing: `ClientAdapter::Send` parses every payload the client sends, only to notice `hello` (Adapters.cxx:140-143). SessionClient built that very json one call earlier.
  - Boundary: the transport has grown six application-protocol sniffers: hello, hello_resp/join_rej, battle_hash/battle_snapshot, the room_action retry kinds (`SameQueuedRoomRetry` decodes `RoomActionMessage`), game_prepared/ready (`IsStaleMatchAck`) and `_terminal_replay`. It also owns `pendingAdmission_`, which belongs to the client's admission protocol. `IsStaleMatchAck` already uses a cheap `find("\"game_")` prefilter, which shows the cost is known but handled ad hoc.
- Proposed change: Classify once, at the edge. Add `std::string type; bool committed; bool verification;` (a `MessageTag`) to `session::Message`. `ReceiveControlMessage` fills it from the `decoded` it already has, and so do the local paths (`ServerAdapter::Send` for connection 1, `ClientAdapter::Send`, `PumpCommittedEffects`). Then:
  - `IsRoomVerificationMessage`, `QueueRoomMessage`, `SameQueuedRoomRetry`, `IsRecoveryBatchableMessage` and `IsStaleMatchAck` read the tag.
  - `AuthorizedEffect` keeps its one parse and hands the resulting json on instead of dumping it back to text. Use a `std::optional<nlohmann::json> body` on Message, set only for messages it delivers. `ClientAdapter::Poll` reads the admission type from the tag, and `SessionClient::Step` uses `message.body` when present (parsing only for transports that do not set it).
  - Replace the hello sniff with an explicit `ClientTransport::BeginAdmission()` called by SessionClient where it builds the hello (Client.cxx:478).
  - Keep `payload` for the wire and for `queuedBytes_` accounting.
- Behavior preserved: Wire bytes stay the same, as do queue order and bounds (byte accounting still uses `payload.size()`). Digest verdicts are unchanged (still SHA over the dump of the payload minus `_commit`). The admission fence opens and closes on the same messages.
- Behavior at risk: Memory, if parsed DOMs were kept in queues: a json DOM is several times its text size and the queue allows 4 MB. So keep only the tag in queues and attach `body` only at drain. Also keep `AuthorizedEffect` rewriting `message.payload` to the form without `_commit`, or move the bound: `HandleGameplayMessage` checks `message.payload.size() > 16384` (Client.cxx:268), and dropping the rewrite would shift that bound by the commit token (about 300 B). No wire or old-client impact.
- Estimated saving: Member, per committed effect of about 13 KB: about 3 parses and 1 dump removed, roughly 1.0-1.5 ms of game-thread CPU (estimate). A commit sends each member a snapshot, a data_update and events. Leader: O(queue) parses per server enqueue go to O(1). Worst case is 64 × ~1 KB parses per push while a quorum round pauses the server, which is when verification traffic queues up. About 60 lines and six ad-hoc classifiers go.
- Confidence: high on the parse counts, medium on the millisecond figures

### F-SESSION-2: A parked effect is re-parsed, re-serialized and re-hashed on every poll, and activation parses the whole client queue
- File: src/session/IrohRoom.cxx:584
- File: src/session/IrohRoomAdapters.cxx:106
- File: src/session/IrohRoomCheckpoint.cxx:246
- Category: overhead
- Severity: P1
- Claim: `AuthorizedEffect` runs `json::parse`, erases `_commit` and calls `PayloadDigest` (a full dump plus SHA-256) at line 586, *before* the activation gate at 591-605. When the gate says "not yet activated" it returns 0, and `ClientAdapter::Poll` breaks with the message still at the head. The comment at 598-603 calls this the normal race: a committed effect beats the checkpoint that activates it. That checkpoint must still cross the pipe (about 460 KB in a 4 × 16 KB credit window, acked once per poll), decode on the worker (11 ms in the baseline) and be imported. So the same head effect is parsed, dumped and hashed on every `SessionClient::Step` for several ticks per commit, for every member. Separately, `ActivateCommittedCheckpoint` (Checkpoint.cxx:246-261) runs `json::parse` on every queued client message (up to 64 and 4 MB) only to look for a `_terminal_replay` marker. Only `PumpCommittedEffects` creates those, locally.
- Proposed change: Decode the `_commit` token first and test `activated`/term before computing the digest. To keep today's verdict exact (a bad digest is dropped at once even while parked), cache the digest outcome on the queue entry: `Message::digestChecked`, set the first time. A digest is a pure function of the bytes, so it never needs recomputing. With F-SESSION-1, the gate becomes integer compares on the tag. In `ActivateCommittedCheckpoint`, mark terminal-replay entries when `PumpCommittedEffects` creates them (or prefilter with `payload.find("\"_terminal_replay\"")`, as `IsStaleMatchAck` does), so other entries are never parsed.
- Behavior preserved: Verdicts (deliver/park/drop), delivery order and which replay messages are removed stay the same.
- Behavior at risk: none identified
- Estimated saving: For each tick an effect stays parked, 1 parse, 1 dump and 1 SHA of the head, about 0.4-0.6 ms for a 13 KB snapshot (estimate). Times the parked ticks (estimated 3-10 per commit) for every member, that is roughly 1-5 ms of game-thread CPU per commit per member. Activation saves up to 64 parses.
- Confidence: high on the code order, medium on how many ticks a head stays parked

### F-SESSION-3: Checkpoint chunks are base64-decoded on the game thread with a linear alphabet search, and copied four times
- File: src/common/CoordinationBytes.hxx:46
- File: src/session/CheckpointTransfer.hxx:52
- File: src/session/CheckpointTransfer.hxx:68
- File: src/session/IrohRoomCheckpoint.cxx:312
- Category: overhead
- Severity: P2
- Claim: Every member receives every commit as about 29 `checkpoint_chunk` events (16 KB raw, 21.8 KB base64url), and the baseline puts each commit at about 460 KB per member. Only the final JSON decode runs on `CheckpointDecodeWorker`. Per chunk, on the game thread:
  - the event parse allocates the 21.8 KB string;
  - `message.at("data").get<std::string>()` copies it;
  - `Decode` calls `alphabet.find(value)` on a 64-character `std::string` for each of the 21.8K characters and appends byte by byte;
  - `bytes_ += decoded` copies again.

  `CompleteReplayAcknowledgment` repeats the decode for retried chunks. On the leader, `PumpCheckpoint` does `substr` (copy), `Encode`, a json member assignment (copy), `dump` (copy) and `Send` (copy), again per 16 KB chunk.
- Proposed change: Give `Decode` a 256-entry `constexpr` reverse table, and have it append straight into the receiver: `bool DecodeAppend(const std::string& text, std::string& out, std::size_t maximum)`. Read the field through `get_ref<const std::string&>()`. On the leader, encode directly from `proposalBytes_.data()+offset` with no `substr`. With F-SESSION-5, base64 leaves the pipe entirely.
- Behavior preserved: Byte-identical output and identical rejections (bad character, size, non-zero trailing bits, `%4==1`).
- Behavior at risk: none identified (a pure function; add a test against the current implementation)
- Estimated saving: The search costs several ms of game-thread CPU per 460 KB commit per member (estimate: 640K searches in a 64-byte string); a table makes it well under 1 ms. About 1.4 MB of copies per received commit go.
- Confidence: high on the structure, medium on the millisecond figures

### F-SESSION-4: Two SHA-256 implementations, and the one used for checkpoints opens a CNG provider on every call
- File: src/common/CoordinationBytes.hxx:15
- File: src/session/RoomDigest.cxx:13
- File: src/session/IrohRoomCheckpoint.cxx:37
- File: src/session/CheckpointTransfer.hxx:64
- File: src/common/ReplaySummaryCache.hxx:53
- Category: judo
- Severity: P2
- Claim: `coordination::Sha256` (a header that includes windows.h and bcrypt.h) calls `BCryptOpenAlgorithmProvider`/`Close` on every call and returns "" on failure. `recovery_detail::Sha256` (RoomDigest.cxx) caches the provider process-wide, is documented as thread safe and falls back to `Sha256Portable`. Proposal digests (leader), `CheckpointReceiver::End` (every member, every commit) and the replay summary cache use the first. Effect digests use the second. This is two canonical helpers for one concept.
- Proposed change: Make one `Sha256Hex(const std::string&)` in common (moved from RoomDigest.cxx) and delete the body of `coordination::Sha256` (forward to it, or rename the call sites). This also takes Windows headers out of CoordinationBytes.hxx.
- Behavior preserved: The same lowercase hex digest.
- Behavior at risk: Only when CNG fails: today `ProposeCheckpointBytes` reports `checkpoint_digest_failed`, and afterwards the portable fallback would succeed. That is arguably better, but it is a change.
- Estimated saving: One provider open/close per proposal, per received commit and per replay summary. About 25 lines and one duplicate concept.
- Confidence: high

### F-SESSION-5: The pipe already carries raw bytes, but payloads cross it as escaped JSON strings and chunks as base64
- File: src/platform/HelperClient.cxx:195
- File: src/session/IrohRoomPeers.cxx:284
- File: src/session/IrohRoomPeers.cxx:330
- File: src/session/IrohRoomCheckpoint.cxx:314
- Category: streaming
- Severity: P2
- Claim: The frame header has a protocol byte (`header[5]==1`) and a length-prefixed binary body, but every room payload is wrapped in another JSON document as a string.
  - Outgoing: `send` puts `{"payload":"{\"_commit\":...}"}`; every `"` is escaped (about +10-20% for snapshot JSON) and the helper must unescape it.
  - Incoming: `message` events do the reverse. The game unescapes the 13 KB payload into the event DOM, then copies it out (`get<std::string>`), then copies it again into the queue (`{peer->first, id, payload, ""}`).
  - Checkpoint chunks are base64url in a JSON string: +33% on the pipe, plus encode on the helper and decode on the game (F-SESSION-3).
- Proposed change: Add a second frame kind (`header[5]==2`): `[compact JSON header]\0[raw body]`, where the header names the body field (`payload` for `send`/`message`, `data` for `checkpoint_chunk` in both directions). The game takes the body as the message payload by move, with no unescape and no copy. Negotiate it: the helper advertises `raw_bodies:true` in its `status` event, and the game uses version 1 until it sees that.
- Behavior preserved: QUIC wire bytes between helpers, checkpoint bytes and digests, and message ordering all stay the same. Remote old clients are unaffected because this is the local pipe only.
- Behavior at risk: Version skew between game and helper (they ship in one package and the bootstrap carries `version==1`). It needs the matching Rust change, so the helper reviewer must sign off.
- Estimated saving: Per commit per member, about 150 KB less pipe traffic (460 KB × 0.33) and no base64 on either side. Per room message, one escape and unescape pass and one or two full copies go. A 16-member leader saves about 25-30 KB of pipe bytes per broadcast (16 × ~13 KB × ~15%).
- Confidence: medium

### F-SESSION-6: The Windows helper-pipe worker polls every 2 ms; the POSIX worker is event-driven; the frame code is copy-pasted
- File: src/platform/HelperClient.cxx:251
- File: src/platform/HelperClient.cxx:147
- File: src/platform/HelperClient.cxx:165
- File: src/platform/HelperClientPosix.cxx:140
- Category: overhead
- Severity: P2
- Claim: `HelperClient::Run` on Windows loops on `WaitForSingleObject(stop_, 2)` + `PeekNamedPipe` for the whole connected lifetime, whether anything is moving or not, even with no room. That is about 500 wakeups/s at 1 ms timer resolution and about 64/s at the default 15.6 ms. `Send` does not wake the worker (POSIX `Send` writes to `wakePipe_`), so every command waits for the next pass. Nothing in src/ calls `timeBeginPeriod`. If the process runs at the default 15.6 ms timer resolution, a pass can take up to a frame, and each checkpoint credit round trip (game ack, helper, next chunk) pays it. `Transfer` creates and closes a kernel event per call (2 per frame read). Each outgoing payload is copied three times: the caller's dump, `Send(const std::string&)` copying into `OutgoingMessage`, and `WriteFrame` copying into a fresh vector. Frame encode/decode and the constants (`MaxPayload`, `MaxMessages`, `HeaderSize`, `IoTimeoutMs`) are duplicated verbatim between HelperClient.cxx and HelperClientPosix.cxx.
- Proposed change: Make Windows match the POSIX design. Keep one overlapped `ReadFile` pending for the 14-byte header on a persistent event, and add an auto-reset `sendWake_` event set by `Send`. Wait with `WaitForMultipleObjects({stop_, sendWake_, readEvent_})`, infinite unless a frame is held back for backpressure (keep the 2 ms retry only then, as POSIX does). Reuse one OVERLAPPED event per direction. `Send(std::string payload)` should take by value and move. Write the header and body as two `Transfer`s (byte-mode pipe) instead of building a vector, and keep wiping the payload after the write. Extract `HelperFrame.hxx` (`EncodeHeader`, `DecodeHeader`, constants) shared by both files.
- Behavior preserved: Frame bytes, ordering, the held-frame backpressure, the 15 s I/O deadline, the auth handshake and wiping all stay the same.
- Behavior at risk: Cancelling the pending overlapped read on `Stop`, and making the auth handshake (id 1) still use the blocking `Transfer`. This runs on the worker thread only, and the game thread gains lower latency.
- Estimated saving: 64-500 wakeups/s and syscalls when idle (depending on timer resolution) drop to about zero. Command and event pipe latency drops from up to 2 ms (up to 15.6 ms at default resolution) to near 0, which shortens checkpoint transfers that are credit-bound. One full payload copy goes on the game thread, plus about 60 duplicated lines.
- Confidence: high on the code, medium on the latency effect

### F-SESSION-7: IrohRoom::Poll is pumped up to four times per game tick by different owners; the transport adapters both pump and drain
- File: src/sf4e/sf4e__NetplayRuntime.cxx:472
- File: src/session/RoomRecoveryRuntime.hxx:13
- File: src/session/IrohRoomAdapters.cxx:24
- File: src/session/IrohRoomAdapters.cxx:41
- File: src/session/IrohRoomAdapters.cxx:103
- File: src/session/IrohRoomPeers.cxx:220
- Category: structural
- Severity: P2
- Claim: `ObserveCoordination`, `RoomRecoveryRuntime::Tick`, `ServerAdapter::Poll`/`PollRecoveryPrefix` (inside `SessionServer::Step`) and `ClientAdapter::Poll` (inside `SessionClient::Step`) each call `IrohRoom::Poll`. Each call re-runs the preamble: decode-result staging, probe deadline, `PruneRetiredPeers`, `ExpireDepartedPeers`, leave/abandon retries and `PumpCheckpoint` with its ScopedTimer. Each also takes up to 8 helper events. So:
  - the per-tick event budget depends on how many owners ran;
  - helper events are consumed in the middle of `SessionServer::Step`;
  - the leader-loss freeze, documented as "observable for one owner tick" (Peers.cxx:220-224), only stops the current `Poll` call. The next owner's `Poll` in the same tick carries on.

  `ServerAdapter::Poll` and `PollRecoveryPrefix` are the same loop copy-pasted (lines 23-38 vs 39-62).
- Proposed change: Add one `IrohRoom::Pump(budget)` per tick, owned by `TickRuntime` (`ObserveCoordination` already pumps first). The adapters and `RoomRecoveryRuntime` drain only. Merge the two server polls into one loop that takes an optional batch predicate. Give the integration harnesses an explicit `Pump`.
- Behavior preserved: Event order, queue semantics and every per-event handler stay the same. Set the budget to 32 to match today's worst case.
- Behavior at risk: Intra-tick timing. Events that today land between the server step and the client step would all land at tick start. The leader-loss freeze becomes a real whole-tick freeze, which is stricter and is what the comment asks for. Tests that rely on adapters pumping need updating.
- Estimated saving: Three redundant preamble passes per tick, about 20 lines of duplicated loop, and one ownership rule instead of four.
- Confidence: medium

### F-SESSION-8: IrohRoom's ~70 fields are reset by hand and repeated snippets stand in for missing types (seams named)
- File: src/session/IrohRoom.cxx:49
- File: src/session/IrohRoom.cxx:29
- File: src/session/IrohRoom.cxx:144
- File: src/session/IrohRoom.cxx:759
- File: src/session/IrohRoom.hxx:350
- Category: judo
- Severity: P2
- Claim: `Begin` resets about 45 members one by one, in two indentation styles; fields have clearly been added piecemeal. The same snippets repeat:
  - close-all-games 5× (IrohRoom.cxx:258, 760, 786, 805, 840);
  - the invitation clear 3× (37, 100, 155);
  - "writable && rebound → Ready" 4× (442, 457, 886, Peers:192).

  The departure machine is 8 fields (`roomCommandQueued_`, `leavePending_`, `leaveAbandon_`, `leaveRetryAt_`, `leaveDeadline_`, `abandonPending_`, `abandonEpoch_`, `abandonRetryAt_`) spread across `Begin`, `Leave` and three separate blocks of `Poll` (766-791, 854-863). The proposal sender is 10 fields (`proposalIdentity_` … `proposalStatus_`), cleared in 5 places (`Begin`, `HandleHelperError` Checkpoint scope, the `coordination_state` term change, the `PumpCheckpoint` abort, `StageCommittedCheckpoint`). The committed-replica state (receiver, staging, decoding, applied/delivered effects, three terminal-replay containers) has its own file, IrohRoomCheckpoint.cxx, but its fields live in IrohRoom.
- Proposed change: The seams:
  - (a) `struct EpochState` for everything `Begin` resets, so `Begin` becomes `epoch_ = EpochState{}`. Process-scoped fields stay outside: `helper_`, `epoch_`, `localIdentity_`, `localUdpPort_`, `network_`, `identity_`, `tournamentAnswers_`, `shortFailures_`, `helperLoad_`, `decoder_`, `nextDecodeTicket_`, `abandon*`.
  - (b) `CheckpointSender` beside the existing `CheckpointReceiver` in CheckpointTransfer.hxx, with `Propose`, `Pump(send)`, `Ack`, `Abort(status)` and `Report`.
  - (c) `Departure`, with `Leave(abandon)`, `Due(now)`, `RetryLater(now)`, `Expired(now)` and `AbandonPending`.
  - (d) `CommittedReplica`, owning `checkpointReceiver_` … `deliveredTerminalReplays_` and exposing `Gate(Message&)`, `Pump(clientQueue)`, `Activate` and `Discard`.
  - (e) Small helpers: `CloseAllGames()`, `ClearInvitations()`, `PromoteIfRebound()`.
- Behavior preserved: Identical. This is a mechanical move.
- Behavior at risk: A field that deliberately survives `Begin` could get reset. Audit it with the list in (a); for example `loadWindow_` is reset today and `helperLoad_` is not.
- Estimated saving: About 60 lines of resets and duplicates. IrohRoom.hxx loses about 40 fields to four small types, and "did Begin reset X" stops being a question.
- Confidence: high

### F-SESSION-9: IrohRoom's dense one-line style hides control flow in a few places and repetition in many
- File: src/session/IrohRoom.cxx:395
- File: src/session/IrohRoom.cxx:538
- File: src/session/IrohRoom.cxx:868
- File: src/session/IrohRoom.cxx:147
- File: src/session/IrohRoomAdapters.cxx:108
- Category: spaghetti
- Severity: P3
- Claim: 340 indented lines use 4-space/no-space style and 441 use tabs. The 4-space blocks are exactly the later coordination, checkpoint, probe and leave-retry code, and the whole of IrohRoomCheckpoint.cxx. Control flow is hidden where a one-line conditional does side effects and then returns:
  - 868: validate, `Fail` and return on one line;
  - 310: close the snapshot, `Fail` and return.

  State transitions are written as multi-assignment lines: 395 (probe start, 7 assignments including a deadline ternary), 147 and 64-71. The probe recommendation (538-539) is one 250-character expression with six conditions and a clamp. In `ClientAdapter::Poll`, `if(allowed>0)` (Adapters.cxx:116) is always true after the `==0 break` and `<0 continue`, so it is a dead conditional. Mostly the density hides repetition (F-SESSION-8), not branches.
- Proposed change: One clang-format pass to the file's majority (tabs) as its own commit, listed in `.git-blame-ignore-revs`. Extract `ProbeSnapshot::Read(event)` and a pure `RecommendedDelay(status, samples, lost, benchmark, recommendation)`. Make `AuthorizedEffect` return `enum class EffectVerdict { Deliver, Park, Drop }` instead of 1/0/-1, and delete the dead conditional.
- Behavior preserved: identical
- Behavior at risk: none identified
- Estimated saving: Legibility. The magic integers leave two files, and the probe rule becomes unit-testable.
- Confidence: high

### F-SESSION-10: SessionClient mixes transport stepping, desync verification and eight boilerplate legacy sends (seams named)
- File: src/session/sf4e__SessionClient.cxx:56
- File: src/session/sf4e__SessionClient.cxx:463
- File: src/session/sf4e__SessionClient.cxx:594
- File: src/session/sf4e__SessionClient.cxx:708
- File: src/session/sf4e__SessionClient__Room.cxx:104
- Category: size
- Severity: P2
- Claim: The file is 836 lines (plus 540 in __Room.cxx). About 300 lines are desync verification v1 and v2: `ReportSnapshotDivergence`, `ReportHashMismatch`, `StrictDesyncEnabled`, `HandleBattleSnapshot`/`Hash`, the pending maps and the 100-line publish/compare block inside `Step` (594-698). About 130 lines are `Lobby_*`, `PreBattle_*` and `Battle_Loaded`, which share one body ("build msg; json j = msg; Send; warn on failure"). `Step` is 240 lines. Its terminal-ack retry loop (486-530) belongs with `AcknowledgeTerminal`/`ReconcileTerminalAcks` in __Room.cxx. Retries are counted in `Step` calls (every 4 or 8 steps, and the join request every 8), so their real cadence follows the frame rate. `MatchResultOutbox` uses milliseconds for the same kind of idempotent retry.
- Proposed change: The seams:
  - (a) `BattleVerification` (sf4e__SessionClient__Verification.cxx) owns the two pending maps and their handlers and exposes `Publish(send, isPlayer)` and `Reconcile()`, so `Step` calls one method.
  - (b) `template <class M> SendResult SendLogged(M msg, const char* what)` collapses the eight legacy bodies.
  - (c) `PumpTerminalAcks()` moves to __Room.cxx.
  - (d) Optionally, express the retry cadences in milliseconds through `GetTickCount64`, as the outbox does.
- Behavior preserved: Identical for (a)-(c). Verification stays read-only on the game thread.
- Behavior at risk: (d) changes retry timing when frame time is not 16.7 ms (loading, minimized, background play). Do it separately, or not at all.
- Estimated saving: SessionClient.cxx goes from about 836 to about 450 lines, with about 90 lines deleted.
- Confidence: high

### F-SESSION-11: RoomAuthority's timestamps mean "instant" while running and "age" while paused, tested by inline ternaries
- File: src/session/RoomModel.cxx:21
- File: src/session/RoomModel.cxx:37
- File: src/session/RoomModel.cxx:100
- File: src/session/RoomModel.cxx:829
- File: src/session/RoomModelJson.cxx:74
- Category: judo
- Severity: P2
- Claim: `readyWaitingSince_`, `startHeldSince_`, `resultPendingSince_`, `lastChatMs_` and `lastActiveMs_` each change meaning with `recoveryPaused_`. About 11 sites (RoomModel.cxx:25, 27, 33, 44, 50, 836, `TimerDue`; RoomModelJson.cxx:74, 79, 153, 321) test `recoveryPaused_` inline to decide which meaning applies. `PauseForRecovery`, `AdvancePausedTimers` and `ResumeRecovery` convert all five families back and forth, and lastChat and lastActive are handled outside `ForEachTableTimer`. This is a missing type, not missing comments.
- Proposed change: Add a `RoomClock`/`PausableStamp` with `Stamp()`, `Age(stamp)` and `Due(stamp, timeout)` that knows whether it is paused. Extend `ForEachTableTimer` into `ForEachTimer`, covering chat and activity with their caps, so Pause, Advance and Resume each become one loop. Keep the serialized form (ages while paused, instants otherwise) exactly as today.
- Behavior preserved: Identical timer outcomes and checkpoint bytes.
- Behavior at risk: Only a more ambitious "virtual clock that stops" variant would change the serialized ages (per-family caps), which older members restore. Do not take that step without a compatibility check.
- Estimated saving: About 11 ternaries become 3 helpers, and one implicit invariant becomes a type.
- Confidence: medium

### F-SESSION-12: RoomModel.cxx (935) and RoomModel.hxx (994) have clean seams that are not yet cut
- File: src/session/RoomModel.cxx:193
- File: src/session/RoomModel.cxx:822
- File: src/session/RoomModel.hxx:30
- File: src/session/RoomModel.hxx:610
- Category: size
- Severity: P3
- Claim: Earlier splits (Actions, Json, ServerOwned, Tournament) worked. What remains in RoomModel.cxx:
  - terminal receipts, about 170 lines (193-360): `Find`/`Remember`/`Store`/`Retire` receipts, `TerminalMembers`, `PendingTerminalEvents`, the rosters;
  - timers, about 170 lines (100-146 and 822-935);
  - seating and set logic (456-724);
  - match lifecycle (724-822).

  The header is about 580 lines of value types plus about 40 inline UI-facing queries (`SeatHold`, `LeavingCost`, `WatchedGameDelay`, `PlaceOf` and others), followed by the 380-line `RoomAuthority`. SessionRecovery.hxx, MemberLatency.hxx and IrohRoom.hxx include all of it but need only the types.
- Proposed change: Move receipts to RoomModelTerminal.cxx, optionally as a `TerminalLedger` value type that owns `terminalReceipts_` and `activeMatchRecipients_`. Move timers to RoomModelTimers.cxx (it pairs with F-SESSION-11). Split the header into RoomTypes.hxx, RoomQueries.hxx and RoomAuthority.hxx.
- Behavior preserved: identical
- Behavior at risk: none identified
- Estimated saving: No file over about 650 lines, and less include fan-out on the x86 build.
- Confidence: high on the seams; the payoff is modest

### F-SESSION-13: BroadcastRoomState deep-copies the full snapshot, chat included, for each member, then discards the chat
- File: src/session/sf4e__SessionServer__Rooms.cxx:391
- File: src/session/RoomModel.cxx:37
- Category: overhead
- Severity: P2
- Claim: For every client on every broadcast, `SnapshotFor` copies `snapshot_` (members, 4 tables, up to 100 chat lines of up to 256 B). For a chat-delta client whose chat is current, line 403 then clears it. Each recipient then gets its own `json(snapshot)` DOM build and a dump/journal of about 13 KB (baseline). Only `localMember`, `localTerminalPending`, `localTerminalGenerations` and `localMatchGenerations` differ between recipients, plus whether chat is left out. Idle, ready and hold values are computed per call but are the same for every recipient within one broadcast. This builds on baseline item 7; it does not propose per-table deltas.
- Proposed change: (1) Cheap: `SnapshotFor(member, bool withChat)` skips the chat copy when it would be cleared. (2) Larger: build the shared part once per broadcast, and encode each recipient with a splice builder that keeps nlohmann's sorted compact key order, the same technique `CommittedEffectWire` and `EncodeSessionProposal` already use. Pre-encode the shared fields once and write the four local fields per recipient.
- Behavior preserved: Payload bytes stay identical (pin this with a test comparing against `json(snapshot).dump()` on the RoomHostBench 16-member fixture), so digests, chat-delta semantics and old clients are unaffected.
- Behavior at risk: (2) must reproduce the exact sorted compact encoding; the test above guards it.
- Estimated saving: (1) about 16 × ~25 KB of chat string copies and allocations per commit on the leader. (2) 15 of 16 DOM builds and dumps of about 13 KB per commit, an estimated 2-4 ms of the 6.3 ms host step in the baseline.
- Confidence: high for (1), medium for (2)

### F-SESSION-14: Per-tick match traffic from this side is small, but the verification relay is the one recurring send
- File: src/session/sf4e__SessionClient.cxx:594
- File: src/session/sf4e__SessionServer.cxx:167
- File: src/session/sf4e__SessionProtocol.hxx:553
- Category: overhead
- Severity: P3
- Claim: During a steady match the game thread sends nothing per frame. Each fighter sends `battle_snapshot` 1/s (`CaptureSnapshot` every 60 frames; `StateSnapshot` is serialized with nested `{"fractional","integral"}` objects, about 1.6-1.8 KB of JSON for a 164-byte struct) and `battle_hash` 2/s (every 30 frames, about 150 B). Spectators only compare. The leader relays each message to the other table participants through a separate `send` command per recipient, each escaped into the pipe (F-SESSION-5), and each hop parses about 8 times (F-SESSION-1). Per table that is about 4 KB/s up to the leader and about (1+S) × 2 KB/s down, over QUIC.
- Proposed change: Do not change the format: NETPLAY_INVARIANTS fixes the snapshot field names, and the absolute cost is small. If F-SESSION-5 lands, add a pipe-level `send_many {recipients:[{peer,control,message_id}], body}` so the leader's relay crosses the pipe once per message, not once per recipient. F-SESSION-1 removes the repeated parses.
- Behavior preserved: Remote wire format and fields unchanged.
- Behavior at risk: none identified (pipe-only)
- Estimated saving: Pipe bytes on the leader cut by (1+S)× for verification, about 2-10 KB/s for a busy 4-table room; CPU is negligible.
- Confidence: medium

### F-SESSION-15: IrohRoom::Game returns a snapshot by value that the match session reads one field of, per link per tick
- File: src/session/IrohRoom.hxx:224
- File: src/session/IrohRoom.cxx:172
- File: src/session/IrohMatchSession.cxx:480
- File: src/session/IrohMatchSession.cxx:584
- Category: boundary
- Severity: P3
- Claim: In Connecting and Started, `IrohMatchSession::Tick` copies `GameSnapshot` (12 integers and two strings) for each link on every tick, only to read `.state`. `StreamSourceClosed` and `RemotePort` do the same. Route strings ("direct", "relay:use1", "unavailable") fit MSVC's small-string buffer and `error` is usually empty, so in practice this does not allocate and the per-frame allocation invariant holds. But that rests on string lengths, not on the API, and an `error` such as "gameplay_peer_closed" (20 characters) would allocate.
- Proposed change: Add `const GameSnapshot* FindGame(const std::string& peer) const` (or `GameState StateOf(peer)`) and use it in IrohMatchSession and NetplayRuntime__Room.cxx:592. Keep `Game()` for tests.
- Behavior preserved: identical
- Behavior at risk: none identified
- Estimated saving: A small copy per link per tick, and the no-allocation guarantee made structural.
- Confidence: high

## Area summary
- **Health:** The game-side session code is careful and heavily commented about invariants, but its data model has not kept up. A room message stays an untyped string through about six layers, so each layer re-parses it to classify it. IrohRoom accumulated about 70 fields with hand-written resets, in two coding styles.
- **Biggest judo move:** F-SESSION-1. Classify once at the edge (tag on `session::Message`, json handed from `AuthorizedEffect` to SessionClient). It deletes six ad-hoc classifiers and the transport's knowledge of hello, admission, verification and retry kinds, and removes about 3-6 parses per message hop with no wire change. F-SESSION-8 is the structural companion: EpochState, CheckpointSender, Departure and CommittedReplica.
- **Biggest overhead items:** All are game-thread CPU per commit per member:
  - F-SESSION-2: the parked head is parsed, dumped and hashed on every poll;
  - F-SESSION-1: 4 parses, 2 dumps and 1 SHA per effect;
  - F-SESSION-3: a linear-search base64 decode of about 460 KB per commit.

  On the pipe, F-SESSION-5 (raw-body frames, -33% on checkpoint chunks) and F-SESSION-6 (the Windows 2 ms poll).
- **Periodic sends:** None of the game-side sends is a large periodic one, and nothing resends a full snapshot on a timer. Broadcasts are event-driven, idle and ready ages are computed only at broadcast time, and `AdvanceCustomRoom` skips a candidate when no deadline is due. The recurring sends found are:
  - `latency_watch` renewal every 5 s (tiny);
  - tournament Claim every 10 s and PermitReady every 2 s until seen (through the helper);
  - verification 1/s and 2/s per fighter (F-SESSION-14);
  - the join_req and terminal-ack retries every 8 Steps until answered.

  Helper to game: `statistics` 1/s per game link, `helper_load` 1/s during games, `coordination_state` on the helper's 1 s tick, and `member_latency` while the board is open. The owner's "large JSON every 15 s" is not on this side. Hand it to the helper and server reviewers (membership publications, the public-room listing and invitation renewal on the helper's statistics tick are the likely places).
- **Rollback invariants respected:** Every proposal keeps `Poll` non-blocking, keeps the decode worker bytes-in/values-out, leaves GGPO callbacks and `bUpdateAllowed` alone, and preserves the one-owner-tick leader-loss freeze (F-SESSION-7 makes it stricter).
- **Could not assess:** No timings were measured (no runs allowed), so all millisecond figures are estimates. The Rust side of the pipe was checked only for cadence. In src/platform, only the helper client and process were reviewed; replay, video and report were not. In netplay/ only SessionController, TournamentPlay and MatchResultOutbox were read; most headers are small state machines and were not reviewed. In common/, only the pipe-adjacent files were reviewed. sf4e__SessionServer*.cxx was read only along the broadcast and verification paths, not reviewed in full.
