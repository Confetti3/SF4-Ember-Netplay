# Adversary verdicts: game-sf4e.md (F-SF4E-1 to 16)

Checked at nightly b9ce0e31, read-only. Design docs read: NETPLAY_INVARIANTS.md, GGPO_LIFECYCLE.md,
SAVESTATE_FREE.md, ROOM_PERF_BASELINE.md (DESYNC_V2 and TRAINING_OVER_ROLLBACK consulted for the v1/v2 and
training points). The GGPO fork source is not in the tree and the IDA server was down, so fork/engine-internal
claims rest on GGPO_LIFECYCLE.md and SAVESTATE_FREE.md.

### F-SF4E-1: PLAUSIBLE  (severity: P2)
- Decisive invariant/evidence: hand diff of the copies.
  - Live `PlayGgpoFrame` (System.cxx:338-417): sync, then `DrainPendingAbort`, then `ClassifyGgpoResult` (transient -> skip
    without advancing; FATAL on a spectator -> `EndSpectatorStream`; FATAL on a fighter -> abort). Then
    `NoteDisconnectFlags`, strip reserved bits x2, then `Simulate(run{guard, install, SyncState, update}, sample, advance)`.
    The guard dies at the end of `run` (System.cxx:380), so the sample and `ggpo_advance_frame` run with playback off.
    Tail: drain, `OnFrameAccepted`, post `SyncState`, `CaptureSnapshot`, meter `show=true`, native result publish.
  - Resim callback (Ggpo.cxx:621-697): inert test (`fatalError || DrainingSpectators()` -> advance and return). Any sync
    failure aborts and still advances (INV-CALLBACK-INERT). No drain (INV-NO-CLOSE-IN-CALLBACK), no `SyncState`
    (INV-PURE-SOUND). The guard at :665 spans sample, advance (and so the nested save), `CaptureSnapshot` and the meter.
  - Orphan (Ggpo.cxx:278-301): guard, zero inputs, `SyncState`, update. No strip, sample or advance.
  - A fourth copy the finding misses: `StressSimulate` (RollbackStress.cxx:176-183): guard, install, update. No `SyncState`.
  The only intentional differences are the sync-failure policy, drain placement, which passes run `SyncState`, and
  the tails. The guard scope is incidental. The only reader of `playbackFrame` is `fSystem::ReadButtons`
  (Pad.cxx:220), which is reached only through the engine's detoured pad getters. Searching the whole repo for
  `GetButtons_|ReadButtons` turns up nothing on the sample, save, `CaptureSnapshot` or `ObserveMatch` path
  (TrainingRuntime.cxx:394 is the offline `BeforeUpdate` path). Nested GGPO work inside a resim should not poll: upstream `DoPoll` is guarded by
  `!_sync.InRollback()`. That was not checked against the fork's own patches (stalled-input-repair, sync-retry-timer). Today the two paths save under different
  playback states, so moving resim to the live placement makes them uniform. I could not verify by reading whether
  any engine memento record reads pads.
- Adjustment: (1) `FramePass` needs a third case, or a `syncSoundBeforeStep` flag. The orphan path also runs the pre-step
  `SyncState` (Ggpo.cxx:298) and the stress harness does not, so a two-value enum would drop one or add one.
  (2) `ReadGgpoInputs` may only synchronize and record diagnostics, returning the raw code. Classification,
  `DrainPendingAbort`, `EndSpectatorStream` and the "abort but still advance" path stay in each caller, and
  `NoteDisconnectFlags` runs after the caller's policy, as it does today. `EndSpectatorStream` must never be reachable
  from the callback. (3) Narrow the resim guard to the update (the live placement), and state that this is inert under
  the current readers. (4) Fold `StressSimulate` in as well. The saving is smaller than claimed. The shared preprocessing
  is two strip lines plus the playback install, so expect about 15-20 lines, not 40. This is a maintenance hazard, not
  a current desync, so P2.
- Duplicate of: no

### F-SF4E-2: PLAUSIBLE  (severity: P2)
- Decisive invariant/evidence: about 25 real game-thread sites. The 31 grep hits include the definition (Status.cxx:658),
  the header and two tests. Publication happens last in `TickRuntime` (NetplayRuntime.cxx:795), after `DrainCommands`
  (:771), so a read is one tick old at a menu. It can be up to 6 ticks old only in a throttled fight
  (PublishCadence.hxx `BackstopTicks = 6`, `padDrivesMenu` at Status.cxx:550), and most of the gates Dispatch reads also
  require `!ggpo`. None of these reads can affect determinism:
  - `BattleUpdate` -> `InputLag::BeforeBattleFrame` acts only on the first frame of a battle no match decided (the
    `decided_` latch, InputLagControl.hxx:29-34). A room match is decided by `StartMatch` before GGPO starts.
  - The `UpdateInputs` gates choose which local input reaches `ggpo_add_local_input`, and GGPO then synchronizes that
    input to both peers.
  - The resim reads `playbackData`.
  The claim that this forces every gate into the published struct "whether the UI needs it" is mostly wrong.
  `canEditPreferences`, `discordCanSwitch`, `canProbe`, `delayLocked`, `canChangeController`, `canReady` and
  `selectedDelay` all have UI readers in src/ui. Only `readyGate` is runtime-only. One site where current vs stale can
  flip an outcome is the Ready/Rematch retry branch (Room.cxx:285-296): `published.localSlot` and `session.room` decide
  whether a parked press is dropped. Some code already chose current state on purpose: `TrainingRequestHolds`
  (Status.cxx:648, "may be newer than the last published snapshot"). Some chose stale on purpose: `NativeLockNow`
  (GameEvents.cxx:183, "the snapshot is the runtime's last tick").
- Adjustment: computing the gates "where Publish runs today" leaves Dispatch exactly one tick stale, as it is now. Only
  the throttle staleness goes away. If the aim is current gates, compute them before `DrainCommands` and again after each
  command that changes state, and list every outcome change, the Ready retry branch included. Tests that drive Dispatch
  through a prior Publish (spectator_runtime_test.cxx:540, :611) need review. The CPU saving (about 25 atomic loads per
  tick) is negligible. The value is consistency only, so P2, not P1.
- Duplicate of: no

### F-SF4E-3: PLAUSIBLE  (severity: P2)
- Decisive invariant/evidence: drain runs inside `PHASE_BATTLE_CLOSING`. `CloseBattle` calls `simGate.OnBattleClosing()`
  and then `NotifyMatchEnded()` (System.cxx:663-666). Two checks test `phase == PHASE_BATTLE_CLOSING` during the drain
  window: `ggpo_free_buffer` (Ggpo.cxx:823) and memento `Fail` (MementoGuards.cxx:45). `DrainingSpectators()` is
  `s_spectatorDrainUntil && fSystem::ggpo` (NetplayFacade.cxx:238). The `&& ggpo` part covers an abort that retires the
  session without clearing the deadline. `localPlayerHandle` is never reset at retirement and is assigned in the middle of
  `StartGGPO`, after `add_player` (Ggpo.cxx:464). The spectator predicates are not one concept:
  - `LocalIsSpectator()` is the room seat (`match->LocalSlot() >= 2`) and exists without a session.
  - Pad.cxx:119 `netplay->spectating` is set from `endpoints.localSlot >= 2` (UserApp.cxx:189) and is also a seat.
  - Only `localPlayerHandle == INVALID` (with a live `ggpo`) is the session role.
- Adjustment: (a) is required to be the reviewer's fallback: draining becomes a flag on top of `PHASE_BATTLE_CLOSING`,
  not a new phase. `OnSessionClosed` clears the flag (that reproduces `&& ggpo`), and so does
  `CancelDeferredGgpoClose`/`ShutdownNetplay`. Keep the inert token and the advance-while-inert behavior exactly.
  (b) Set `SessionRole` where the handle is assigned, not at entry, and reset it in `RetireGgpoSession`. Every site that
  reads it today is gated on `ggpo` or runs inside a callback, so the reset does not change any of them. Do not fold
  `LocalIsSpectator()` into the role: it answers seat questions before and after the session.
  `netplay->spectating` can stay a seat flag. It agrees with the role while `ggpo` is live, but `BackgroundPlay` reads
  it as "watching", which is a seat concept. No overhead win, so P2.
- Duplicate of: no

### F-SF4E-4: PLAUSIBLE  (severity: P2)
- Decisive invariant/evidence: Room.cxx:82-210 is a sentinel-ordered chain as described, and the generation check at :92
  sits after identity/tournament. Two facts limit a plain variant:
  - Some members are payloads of several families, not commands in their own right. `discordRevision` selects the
    invite family (:107) and is also a payload re-validated on room commands (`if (command.discordRevision)`,
    :214-222; set on JoinInvite/LeaveRoom by `PublishAndTickDiscordInvite`, Status.cxx:617-626). `preferences` rides on
    HostRoom, JoinInvite and SavePreferences.
  - The hidden-shell submit (Overlay.cxx:267-273) fills `command`, `identity`, `publicTicket` and `createdRules` in one
    object, and identity wins because it is tested first.
  Parked intents (`readyIntent`, `roomActionIntent`, `lobbyEditIntent`) store a whole `RuntimeCommand` and re-dispatch it.
- Adjustment: `RoomCommand` must carry `discordRevision`, `preferences`, `publicTicket`, `createdRules`, `join`,
  `practiceRoom`, `delayPolicy`, `character`, `stage`, `randomPool`, `roomAction` and `selectedDelay` as payload. The
  `ShellAction` -> variant conversion must keep identity winning over a room command. The parked intents must hold the
  variant (or only `RoomCommand`). The mailbox byte cost is supplied by the caller (BoundedMailbox.hxx:17), so the sizing
  stays the caller's job. Pure refactor, no overhead change, so P2.
- Duplicate of: no

### F-SF4E-5: CONFIRMED  (severity: keep)
- Decisive invariant/evidence: on every publish, `FillRoomView` copies the room by value
  (`snapshot.room = client.GetRoomSnapshot()`, Status.cxx:69) and then mutates the copy (badge strip, :74-75, keyed by
  `ServerOwned()` and `preferences.classEmblems`). `DrawApplicationHome` copies it again per drawn frame and patches idle
  and hold fields in place (Overlay.cxx:283-300). `SessionClient` replaces `_roomSnapshot` only by whole assignment
  (sf4e__SessionClient.cxx:236, SessionClient__Room.cxx:397, :435), and grep found no in-place writes, so an immutable
  shared copy per accepted revision is feasible. Payload sizes are consistent with ROOM_PERF_BASELINE (13 KB without
  chat, chat two thirds), and this is in-process only, so it does not contradict the "no per-table wire deltas"
  reasoning. `GetRoomSnapshot()` returns a reference, so game-thread readers such as `TrainingRequestHolds` are
  unaffected.
- Adjustment: the badge strip means the published pointer is a memoized derived copy, rebuilt when (room revision,
  roomEpoch, `classEmblems`, `ServerOwned()`) changes. That is one copy per change, not zero. Rebuild `members` with it.
  The chat carry-over at SessionClient__Room.cxx:388-389 can share the previous chat vector instead of copying it. For
  the draw-side half, use F-UI-4's reader-side elapsed-time plan (IdleText, HoldStatus, DescribeReadyDeadline). The
  saving estimate stands. It only applies while a room is joined and the menu is shown.
- Duplicate of: canonical. F-UI-4 (game-ui.md) is the same room deep copy seen from the drawing side, so fold it in here.

### F-SF4E-6: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: three pointer-keyed `std::map`s (Game__Battle.hxx:88-93). `SyncState` runs twice per live
  frame. Counted lookups per `SyncState`: stub `bLive` + real `bLive` + the stop loop + reconcile, about 4 x 64, plus a
  pairwise compare over live sounds only and queued stops x realCount. Save adds 64 `find`s (SaveState.cxx:813-826, stub
  adapters only), plus `IsStillPlaying`. That is about 600-700 per frame, not 1,000. At roughly 30 us per frame the
  saving is real but small. The stub metadata is simulation-visible: `IsStillPlaying` returns stub `bLive`
  (Game__Battle.cxx:510-515), and it is saved and restored, so INV-SAVE-ROUNDTRIP applies to it. The reviewer's
  statement that iteration order is presentation-only is correct: Load restores by key, and per-manager real starts and
  stops touch only that manager's real adapters.
- Adjustment: the per-manager save record must mirror today's stub-only record. An unknown adapter in `IsStillPlaying`
  must return false (today `operator[]` inserts `bLive=false`). The developer overlay readers
  (DeveloperOverlay__System.cxx:246, :335) must be migrated. The destructor still has to drop its record, and that
  replaces the purge. Lower to P3 on measured weight.
- Duplicate of: no

### F-SF4E-7: CONFIRMED  (severity: keep)
- Decisive invariant/evidence: `fKey::Initialize`/`ClearKey` are Detours entry patches (Game.cxx:36-55). Native
  Initialize calls ClearKey (SAVESTATE_FREE.md:50), so the internal call lands in the detour, which erases
  unconditionally, and then Initialize re-inserts. `SaveState::Save` memsets every tracked key (SaveState.cxx:790-799), so
  each record re-initializes about 90 keys. That is a free plus a malloc for each key, on every save. `FreeDetached` uses
  the trampoline ClearKey (SAVESTATE_FREE.md:99) and causes no set churn.
- Adjustment: the minimal version is safe without the counter. `NoteEngineClear` still runs, membership after
  Initialize is identical, and nothing iterates `trackedKeys` during the synchronous native call (CheckKeyWrite runs
  only in Load/Free). If the routing does not exist, the guard is simply never hit. Two requirements:
  - Key the skip on pointer identity (`this == initializingKey`, saved and restored by RAII so a nested Initialize of
    another key is handled), not on a bare depth counter. A depth counter would also suppress the erase of other keys
    cleared during an Initialize.
  `fKey::Initialize` always re-inserts after the native call (Game.cxx:51), so membership afterwards cannot differ.
  The insert counter is still useful, but only to confirm the size of the saving, not for safety. The flat-container
  "better" option is fine if it keeps address order.
- Duplicate of: no

### F-SF4E-8: PLAUSIBLE  (severity: keep)
- Decisive invariant/evidence: the companion exits after 15 s with no accepted presence (main.cxx:67). `lastInput` is
  refreshed only after the `roomEpoch < epoch` skip (:58-62). It is started once per launch and never restarted
  (NetplayRuntime.cxx:363-366), so an exit lasts for the whole session. The 15 s value has been there since 0.8.0
  (`git log -S15000`) and the companion ships with the game, so there is no older-companion timeout to worry about.
  `PublishDiscordPresence` runs inside `Publish`, so even a throttled fight publishes at least every 6 ticks, and the
  1 s timer holds. A cached resend every 5 s keeps the companion alive with about 10 s of tolerance for game-thread
  stalls (14 s today). The real problem is that today's 1 s rebuild also refreshes inputs that are not in the 8-word key:
  - `runtime->room->DiscordInvitation()`, which a helper renewal (IrohRoom.cxx:869) changes without a room revision.
  - `netplay::tournament::HoldsMatchRoom(tournament, watch)` (Status.cxx:222), which clears the secret.
  - `snapshot.room.serverOwned`.
  With the cache, a renewed invitation, or a tournament/watch transition that must hide the secret, would not reach
  Discord until something else changed the key. That is a behavior change, and for the tournament case a privacy
  regression.
- Adjustment: add a hash of `DiscordInvitation()`, the `HoldsMatchRoom` bit and `serverOwned` to the key, or keep a full
  rebuild at the heartbeat. Then the cached resend at 5 s is safe. Changing `PresenceInput::room` to a const pointer is
  fine. Keep the `presence` type only.
- Duplicate of: no

### F-SF4E-9: PLAUSIBLE  (severity: keep)
- Decisive invariant/evidence: `CaptureMenuInput` calls `OverlayPrefs::Load` on every tick in every state, fights included
  (NetplayRuntime__Input.cxx:122, outside the `AtMainMenu` block). When `settings.json` has no `overlay` object, `Access`
  returns null and `Load` returns false without caching (OverlayPrefs.cxx:216-224, SettingsStore.cxx:137-141). Each tick
  then does `create_directories`, `CreateFileW(settings.lock)` and a full read and parse on the game thread. Only
  SettingsStore/SettingsWriter write `overlay`; the launcher never does. So on a fresh install the miss lasts until the
  game first queues an overlay save, and `SaveGameplayDevice` short-circuits when the device equals the default `cached`
  (OverlayPrefs.cxx:316). The keyboard scan result is read in Idle as well: `padInventory.Build` sets the keyboard row's
  `pressed` from `device.buttons` (ControllerInventory.hxx:50, :67) every tick, and `InputAssignment::Tick` reads it during
  capture (InputAssignment.hxx:41-56).
- Adjustment: make the negative cache time-bounded, retrying every few seconds, rather than "until Save". A transient
  lock failure while the launcher writes would otherwise pin defaults. Gate the keyboard scan on "capture not Idle, or
  the Controllers page shown", and in the same gate stop `Build` from reporting a keyboard `pressed` it no longer
  measures. `DeviceIdentity()` is a straightforward win. P2, because the cache-miss path can put disk I/O inside a
  netplay fight tick.
- Duplicate of: no

### F-SF4E-10: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: `ResetMatchEntry` (NetplayRuntime.cxx:30-39) and `CloseRoom` (:93-136) reset overlapping,
  hand-listed sets. `CloseRoom` does all its resets before `ShutdownNetplay(true)` and `room->Leave`, which have side
  effects.
- Adjustment: keep the `= {}` group resets ahead of `ShutdownNetplay`, as the field resets are today, because the shutdown
  path calls back into the facade. `ResetMatchEntry` also clears the global `VsBattle::nextRequest`. Purely structural
  with no runtime effect, so P3.
- Duplicate of: no

### F-SF4E-11: PLAUSIBLE  (severity: keep)
- Decisive invariant/evidence: Ggpo.cxx:376-555 is as described. `RetireGgpoSession` calls `matchTelemetry.Reset()`
  (:239), so "retire first, then `Reset(spectator)`" reproduces both of today's end states. The fighter double-abort is
  real: `StartGGPO` aborts with `runtime.ggpo_start_failed` or `ggpo_add_players_failed`, then `_OnVsBattleTasksRegistered`
  sees `ggpo == null` (UserApp.cxx:249, :267-268) and aborts again with `authorized_connection_unavailable`.
- Adjustment: keep the invalid-roster early return (:379-382) ahead of `CancelDeferredGgpoClose` and the leftover retire.
  On spectator failure, `nNextBattleStartFlowTarget` and `bTerminateOnNextLeftBattle` are still set today (:552-553) but
  not on fighter failure. Pick one deliberately and keep it. State the notice change as intended. Keep the per-call
  difference that only `StartGGPO` applies `GgpoDisconnectTolerance` (NETPLAY_INVARIANTS: fighter links only).
- Duplicate of: no

### F-SF4E-12: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: `CopyIntoPlace`/`Clear` are non-static (SaveState.cxx:321, :397) with no users in other
  files, so making them file-local is safe. The five open latches (Overlay.cxx:503-540) are not uniform:
  - `inviteShown` is a bool that resets when not pending and skips the `Available()` check.
  - `replayLinkShown` is a string, clears on empty and requires `room == Idle`.
  - `roomLinkShown` advances even when it does not open (it opens only if `free`).
- Adjustment: the file moves are CONFIRMED as pure moves. The trigger table needs a per-entry key type, a reset rule and an
  "advance without opening" flag, or those three stay hand-written. Size only, so P3.
- Duplicate of: no

### F-SF4E-13: PLAUSIBLE  (severity: keep)
- Decisive invariant/evidence: the ten flow globals are listed by hand in Save (:863-872), CopyIntoPlace (:329-338) and
  Clear (:425-434), so the `BattleFlowGlobals` struct is a safe, byte-identical change. `CaptureSnapshot` (:245-300)
  re-reads 18 getters that `CaptureCharaSemantics` already reads. However, the trace row is keyed by GGPO state frame
  while `CaptureSnapshot` keys by the wrapping engine `frameIdx % 60`. `CaptureSimulatedFrame` returns early when
  `stateFrame <= 0` (:219) where `CaptureSnapshot` does not. And the two run at different points (before vs after
  `ggpo_advance_frame` and the post `SyncState`).
- Adjustment: do the globals struct. Drop or defer the "build v1 from the trace" half: it saves 36 getter calls per second
  and puts a bit-exact, peer-`memcmp`'d payload at risk (NETPLAY_INVARIANTS: v1 stays active). Retiring v1 stays a
  separate decision gated on DESYNC_V2. That decision also involves room hosts that forward `battle_snapshot`.
- Duplicate of: no

### F-SF4E-14: PLAUSIBLE  (severity: keep)
- Decisive invariant/evidence: `Catalog` is `std::unordered_map<std::string, std::string>` (Localization.hxx:15), and
  `Lookup` calls `find(const char*)` (Localization.cxx:181), which builds a temporary string. The ids exceed the SSO size.
  `Steam_PostUpdate` resolves the text every tick (UserApp.cxx:427-431).
- Adjustment: `sf4e_localization` builds at C++17 (CMakeLists.txt:247 `cxx_std_17`; the global setting is 11 at :112;
  nothing in the tree asks for C++20). Transparent lookup on `unordered_map`
  is C++20, so "make Catalog use a transparent hash" is not available. Use `std::map<std::string, std::string, std::less<>>`,
  a `string_view`-keyed index built at parse time, or simply resolve on the edge (the `ObserveControlPlane` part, which
  is free). Negligible either way.
- Duplicate of: no

### F-SF4E-15: CONFIRMED  (severity: keep)
- Decisive invariant/evidence: 133 `(unsigned int)s + 0x...` accessors in src/Dimps, and no IPO, /GL or /LTCG in
  CMakeLists.txt, CMakePresets.json or cmake/. Inlining keeps the addresses identical. The runtime gain is negligible:
  a few nanoseconds per call and a handful of calls per frame. The value is the line count.
- Adjustment: none. Keep out-of-line any accessor whose address is taken. A rough grep for `&X::Get...` outside the
  method tables found none, but that was not checked exhaustively.
- Duplicate of: no

### F-SF4E-16: REJECTED  (severity: keep)
- Decisive invariant/evidence: the per-draw `s_prefs` copy and compare (Overlay.cxx:798-803) is about 2 KB, roughly
  120 KB/s of memcpy. The whole-struct compare catches every edit path by construction, while a dirty flag must be set at
  each edit site (the selector callback, `selectionStep`, controller paths). The presentation envelope
  (NetplayPresentation.cxx:102-111) saves one allocation per tick. Its fingerprint would have to cover `pingMs`
  (`matchTelemetry.Ping(now)`, time-dependent), the countdowns and notice expiry, which means computing most of
  `GetStatus()` anyway.
- Adjustment: none. The saving is negligible, and the change adds a way to miss an edit.
- Duplicate of: no

## Batch verdict
- Counts: CONFIRMED 4 (5, 7, 10, 15); PLAUSIBLE 11 (1, 2, 3, 4, 6, 8, 9, 11, 12, 13, 14); REJECTED 1 (16).
  Severity changes: 1 P1->P2, 2 P1->P2, 3 P1->P2, 4 P1->P2, 6 P2->P3, 10 P2->P3. 9 keeps P2, but the reason is disk I/O in
  fight ticks on the cache-miss path.
- Do first, all overhead with no determinism surface:
  1. F-SF4E-7 minimal (identity-keyed nested-ClearKey skip). Small, safe as specified, and removes per-save allocator
     churn inside GGPO callbacks, which multiplies in rollbacks.
  2. F-SF4E-9 (`DeviceIdentity()` plus a time-bounded negative cache). It removes a possible settings.json read and parse
     on every game-thread tick, fights included.
  3. F-SF4E-5 together with F-UI-4. This is the largest in-process copy volume while the menu is shown in a room.
  Then F-SF4E-8, with the key widened as described.
- The P1 structural items (1, 3, 4) bring no overhead win and touch the determinism and lifecycle surface. Do them as
  separately reviewed refactors with the adjustments above, not as part of the overhead pass.
- Not verifiable by reading:
  - Whether any engine memento record path reads pads (F-SF4E-1 guard scope). IDA was down.
  - The real count of nested ClearKey per save (F-SF4E-7 magnitude).
  - Whether a fresh install actually stays on the `Load` miss path for long (F-SF4E-9).
  - The GGPO fork's in-rollback DoPoll behavior: the upstream guard is assumed, the fork patches were not read.
