# Findings: src/sf4e/ and src/Dimps/ (game hooks, rollback, overlay, runtime)

Reviewed at nightly b9ce0e31. Read: all of System.cxx, Ggpo.cxx, SaveState.cxx, System.hxx, System__Internal.hxx,
FrameTrace.cxx, MementoGuards.cxx, Game.cxx, Game__Battle.cxx (sound shadowing), Overlay.cxx, NetplayPresentation.cxx,
NetplayFacade.cxx (lifecycle part), NetplayRuntime.cxx, NetplayRuntime.hxx, NetplayRuntime__Status.cxx,
NetplayRuntime__Room.cxx (Dispatch), parts of __Input, Pad.cxx, UserApp.cxx, OverlayPrefs.cxx, InputDevices.cxx,
InputLag.cxx, Dimps__Game__Battle__System.cxx, Dimps__Event.cxx, Dimps__Pad.cxx; plus common/MementoGuards.hxx,
common/sf4e__GgpoGate.hxx, netplay/PublishCadence.hxx, discord/main.cxx, discord/Presence.hxx and the five design docs
plus docs/perf/ROOM_PERF_BASELINE.md. Nothing was built or run; all costs below are estimates from the code.

Invariant names used below (from NETPLAY_INVARIANTS.md / GGPO_LIFECYCLE.md / DESYNC_V2.md):
- INV-UNDETOURED: the advance-frame callback calls the undetoured `rSystem::BattleUpdate`.
- INV-INPUT-SYMMETRY: a live frame and its re-simulation must feed the engine the same inputs, prepared the same way.
- INV-MAIN-THREAD: Save/Load/Free and every engine memento call stay on the game thread.
- INV-CALLBACK-INERT: inside a fatal or draining session, callbacks touch no engine state but keep GGPO's frame count moving
  (advance still called, save hands out the inert token).
- INV-NO-CLOSE-IN-CALLBACK: never `ggpo_close_session` from a callback.
- INV-PURE-SOUND: `SyncState` around the outer simulation step only, never per re-simulated frame.
- INV-SAVE-ROUNDTRIP: what Load restores is byte-identical to what Save recorded.
- INV-ORPHAN: a claimed netplay battle never runs the native pad path.

### F-SF4E-1: The GGPO frame step exists three times (live, re-simulation, orphan) and must be kept identical by hand
- File: src/sf4e/sf4e__Game__Battle__System.cxx:338
- File: src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:621
- File: src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:278
- Category: judo
- Severity: P1
- Claim: `PlayGgpoFrame` (live) and `ggpo_advance_frame_callback` (rollback) are two hand-maintained copies of the same
  deterministic step: `ggpo_synchronize_input` into `fPadSystem::Inputs[2]` -> `NoteDisconnectFlags` -> `ClearReservedInputBits`
  on both sides -> `PlaybackFrameScopeGuard` + `playbackFrame = 0` + `playbackData[0][0..1]` -> engine `BattleUpdate` ->
  `CaptureSimulatedFrame` -> `ggpo_advance_frame` via `GgpoTimeline::Simulate` -> `CaptureSnapshot` / `CaptureMeterSample`.
  The callback's own comment ("As in a frame of the present (PlayGgpoFrame): the game is never shown the reserved bits",
  Ggpo.cxx:658) admits the coupling. They already differ in incidental ways (the guard is inside the Simulate lambda in
  one and outside it in the other). `LeaveOrphanedNetplayBattle` (Ggpo.cxx:294-301) is a third copy of the
  "install playback inputs, optionally SyncState, run the engine update" block with neutral inputs. Any future edit to
  input preprocessing (another reserved bit, a Practice-only mask, a training override) has to land in all three or the
  live frame and its re-simulation read different inputs, which is a desync, not a crash.
- Proposed change: one file-local core in System.cxx, e.g.
  `GGPOErrorCode SimulateWithInputs(rSystem*, fPadSystem::Inputs in[2], FramePass pass)` where `FramePass` is
  `{Live, Resim}`; it does `ClearReservedInputBits` x2, installs playback under the guard, runs the pre-step `SyncState`
  only for `Live`, runs the engine update, and returns `GgpoTimeline::Simulate(...)`'s advance result. A second helper
  `ReadGgpoInputs(Inputs out[2]) -> GGPOErrorCode` owns synchronize + diag record + `NoteDisconnectFlags`.
  `PlayGgpoFrame` keeps only its live-only tail (OnFrameAccepted, post `SyncState`, result publish, meter `show=true`);
  the callback keeps only its tail (rollbackHud, window counters, meter `show=false`). `LeaveOrphanedNetplayBattle` calls
  the install-and-update part with zero inputs (no advance).
- Behavior preserved: identical call order per pass; the callback still calls the undetoured method; the orphan path
  still re-asserts RS_ISLEAVING before its update.
- Behavior at risk: one real divergence must be settled: the live path ends playback mode inside the `Simulate` lambda,
  before `ggpo_advance_frame` (System.cxx:377-380: the nested save and any poll must not read stale playback inputs),
  while the callback's guard spans the whole callback, so playback mode is still on during its nested
  `ggpo_advance_frame` save. The live placement should win (narrow the resim window to the engine update); that changes
  only what a nested save/poll inside a resim could read, which per the live comment it must not. Keep the pre-step
  SyncState on Live only. The shared helpers must never call `DrainPendingAbort` (INV-NO-CLOSE-IN-CALLBACK); only the
  live caller drains after its top-level GGPO calls. Must respect INV-UNDETOURED, INV-INPUT-SYMMETRY, INV-PURE-SOUND,
  INV-ORPHAN.
- Estimated saving: ~40 lines deleted; removes the one place where a live/resim divergence can be introduced by editing
  only one copy.
- Confidence: high

### F-SF4E-2: Game-thread decisions read back the presentation snapshot the same thread published (28 call sites)
- File: src/sf4e/sf4e__NetplayRuntime__Room.cxx:110
- File: src/sf4e/sf4e__NetplayRuntime__Room.cxx:164
- File: src/sf4e/sf4e__NetplayRuntime__Room.cxx:269
- File: src/sf4e/sf4e__NetplayRuntime__Room.cxx:448
- File: src/sf4e/sf4e__NetplayRuntime.cxx:661
- File: src/sf4e/sf4e__NetplayRuntime__Input.cxx:160
- File: src/sf4e/sf4e__Pad.cxx:110
- File: src/sf4e/sf4e__Game__Battle__System.cxx:444
- File: src/sf4e/sf4e__NetplayRuntime__Status.cxx:404
- Category: boundary
- Severity: P1
- Claim: `RuntimeSnapshot` is the drawing thread's view model, but 28 game-thread sites (`grep GetRuntimeSnapshotShared`)
  use it as the source of truth for gates: `Dispatch` checks `canEditPreferences`, `discordCanSwitch`, `canProbe`,
  `delayLocked`, `readyGate`, `localSlot`; `CallOutOfTraining` reads `canReady`; `CanChooseControllerNow` reads
  `canChangeController`; `Pad::UpdateInputs` reads `menuContext`, `preferences`, `inputDevice`; `BattleUpdate` reads
  `preferences.lowerInputLag` every frame. `TickRuntime` publishes last (`PublishAndTickDiscordInvite`), so every read
  is at least one tick old, and in a throttled fight (`PublishCadence`, BackstopTicks = 6) up to six. The gates are
  computed in `Publish`/`FillRoomView` from live runtime state and then consumed back through an atomic shared_ptr load:
  a producer/consumer loop inside one thread, with the staleness hidden. It also forces every gate into the published
  struct whether the UI needs it or not.
- Proposed change: extract a pure `Gates ComputeGates(const Runtime&, const netplay::Snapshot&, const room::Snapshot&,
  bool atMainMenu)` (the body of the gate half of `FillRoomView`, `FillLockReasons`, `canOpenRoom`, `canEditPreferences`,
  `discordCanSwitch`, `canChangeController`, `canTrain`) and compute it once per tick where `Publish` runs today, storing
  it on `runtime`; that same object is what `Publish` copies, so both readers see identical values (recompute after
  `DrainCommands` if a command changed state). Game-thread consumers read
  `runtime->gates`; `Publish` copies `runtime->gates` into the snapshot. `BattleUpdate` and `UpdateInputs` read
  `runtime->preferences` directly (both run on the game thread).
- Behavior preserved: the UI sees the same gate values; the same rules decide every command.
- Behavior at risk: gates become current instead of one to six ticks stale. A command racing a state change by one tick
  could now be accepted/refused differently. That is a correctness improvement but should be stated in the change.
- Estimated saving: removes 28 atomic shared_ptr loads/refcount pairs per tick-path (small CPU); main win is one
  canonical place for "may the player do X now".
- Confidence: high

### F-SF4E-3: Session role and drain phase are re-derived in ~25 places with four different predicates
- File: src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:638
- File: src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:702
- File: src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:735
- File: src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:837
- File: src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:103
- File: src/sf4e/sf4e__NetplayFacade.cxx:238
- File: src/sf4e/sf4e__Pad.cxx:119
- File: src/sf4e/sf4e__NetplayRuntime.cxx:44
- Category: spaghetti
- Severity: P1
- Claim: "Am I a spectator?" is answered four ways: `localPlayerHandle == GGPO_INVALID_HANDLE` (12 sites across System,
  Ggpo, SaveState), `UserApp::netplay->spectating` (Pad.cxx:119), `LocalIsSpectator()` = `match->LocalSlot() >= 2`
  (runtime), and `matchTelemetry.spectator`. "Am I P1 hosting spectators?" is `players[0].handle == localPlayerHandle`
  (Ggpo.cxx:106). "Is the session draining spectators?" is `NetplayFacade::DrainingSpectators()` = `s_spectatorDrainUntil
  && fSystem::ggpo` (10 sites), a facade static that the battle hook layer consults from inside three GGPO callbacks next
  to `simGate.fatalError` (`if (simGate.fatalError || DrainingSpectators())` x3). That is session-phase state of the
  battle layer living one layer up, so the hooks depend on the facade to know their own phase (layering inversion), and
  each callback repeats the inert-session test by hand.
- Proposed change: (a) add `PHASE_DRAINING_SPECTATORS` to `GgpoGateModel` (entered by `NotifyMatchEnded` when it defers,
  left by `RetireGgpoSession`), keep `s_spectatorDrainUntil` only as the deadline, and give the gate one predicate
  `CallbacksInert() const { return fatalError || phase == PHASE_DRAINING_SPECTATORS; }` used by the three callbacks and
  `DrainingSpectators()` (which becomes `simGate.phase == PHASE_DRAINING_SPECTATORS`). (b) Add `enum class SessionRole
  { None, HostFighter, Fighter, Spectator }` to `fSystem`, set in `StartGGPO`/`StartSpectating`, reset in
  `RetireGgpoSession`; `IsSpectatorHandle`, `SpectatorStreamCount`, `SimulatedStateFrame`, `CaptureMeterSample`,
  `PlayGgpoFrame`'s fatal branch, `AbortGgpoMatch`, `PollMatchTelemetry`, `NetplayWindow::Flush` and the event callback
  read the role. `Pad.cxx` and the runtime read the same role while a session exists.
- Behavior preserved: same truth values in every state reachable today (the role is fixed for a session's life, as the
  handle is).
- Behavior at risk: the drain phase must not hide `PHASE_BATTLE_CLOSING` uses: `ggpo_free_buffer` and memento `Fail()`
  test `phase == PHASE_BATTLE_CLOSING`; draining happens after `OnBattleClosing`, so either keep draining as a flag on top
  of BATTLE_CLOSING or update those two tests. Must keep INV-CALLBACK-INERT exactly (advance still called, inert token
  still handed out).
- Estimated saving: ~25 scattered predicates become 2 named concepts; removes the facade dependency from the callbacks.
- Confidence: high

### F-SF4E-4: `Dispatch` is a 420-line if-chain over a bag-of-optionals command type
- File: src/sf4e/sf4e__NetplayRuntime__Room.cxx:82
- File: src/sf4e/sf4e__Overlay.cxx:424
- File: src/sf4e/sf4e__Overlay.cxx:267
- Category: structural
- Severity: P1
- Claim: `RuntimeCommand` carries ~25 independent optional members (`identity.op`, `tournament.op`, `training`,
  `leaveWatchedGame`, `discordAction`, `previewSoundVolume >= 0`, `replay.mode`, `shortInvitation`, `padAction.kind`,
  `inputAction`, `service`, `steamProfile`, `roomAction`, `preferences`, `character`, ...). Which one is "the" command is
  decided by the order of the `if` chain in `Dispatch` (lines 83-200: twelve families tested by sentinel values), followed
  by room-command validation (200-345) and an effect switch (345-500). The overlay rebuilds a `RuntimeCommand` from a
  `ShellAction` field by field (Overlay.cxx:424-450, and again 267-273 for the hidden shell), so every new UI action touches
  three places and the sentinel test order is load-bearing (a command with two members set silently takes the first).
- Proposed change: make the command a `std::variant` of typed commands (`IdentityCmd`, `TournamentCmd`, `TrainingFromRoom`,
  `LeaveWatchedGame`, `DiscordInviteCmd`, `PreviewSound`, `ReplayCmd`, `ShortInvite`, `PadCmd`, `InputCmd`, `ServiceCmd`,
  `RoomCommand{netplay::Command, payload...}`); `ShellAction` produces the same variant so the overlay moves it through
  unchanged. `Dispatch` becomes `std::visit` over one handler per alternative; only `RoomCommand` goes through the
  controller/validation/effect half, which itself splits into `ValidateRoomCommand` and `ApplyEffect`. Move the handlers
  to `sf4e__NetplayRuntime__Commands.cxx`.
- Behavior preserved: each family's handler body is moved unchanged; the controller-generation check stays where it is
  for room commands.
- Behavior at risk: the generation check (line 92) currently runs before the training/watch/discord/service families
  too; keep it in exactly the handlers that pass it today. The mailbox byte accounting (`CommandMailbox(32, 128 KiB)`)
  must size the variant.
- Estimated saving: Room.cxx 880 -> ~450 lines; the 25-line field copy in Overlay.cxx disappears; sentinel ordering
  disappears as a concept.
- Confidence: high

### F-SF4E-5: The whole room (with up to 100 chat lines) is deep-copied every tick into RuntimeSnapshot and again every drawn frame into ShellView
- File: src/sf4e/sf4e__NetplayRuntime__Status.cxx:404
- File: src/sf4e/sf4e__NetplayRuntime__Status.cxx:67
- File: src/sf4e/sf4e__NetplayRuntime__Status.cxx:546
- File: src/sf4e/sf4e__Overlay.cxx:276
- File: src/sf4e/sf4e__Overlay.cxx:283
- Category: overhead
- Severity: P2
- Claim: `PublishThrottled` publishes every tick whenever no GGPO session exists or the pad drives Ember's menu
  (`padDrivesMenu`, Status.cxx:550): the main menu, rooms, character select and every offline battle. Each `Publish`
  builds a fresh `RuntimeSnapshot` by value: `room = client.GetRoomSnapshot()` (members, 4 tables with spectator/queue
  vectors, `chat` vector of up to `MaximumChatMessages` = 100 strings of up to 256 B), `members` (rebuilt
  `MemberView`s), `preferences` (incl. `controllerMappings` map and `buttonLayouts`), `pads` (with `buttonNames`
  strings), `publicRooms`, `rankings`, `tournament`, `memberLatency`, plus ~25 strings, then `make_shared`. The overlay
  then copies `snapshot.room`, `members`, `publicRooms`, `rankings`, `pads`, `preferences`, `tournament`, `replays` into a
  `ShellView` on every drawn frame while the menu is visible (Overlay.cxx:277-384), and formats `selectionSummary`,
  `ultraName`, `appearanceName`, `stageName`, `fighterOptionsName` with `loc::Tf` each frame. The author already avoided
  this on the hidden path ("not copied, since this runs every match frame", Overlay.cxx:256-259), so the cost is known.
  Payload: in-process copy (no wire). ROOM_PERF_BASELINE puts a busy 16-member room at ~13 KB without chat and chat at
  two thirds of a snapshot, so ~35-40 KB of room per copy, on the order of 150-300 heap allocations (member strings, 100
  chat strings, table vectors). Interval: every outer tick (60 Hz) on the game thread, plus every draw frame (60 Hz)
  while the menu is shown. Fan-out 1 (local). Transport: none (shared_ptr hand-off to the drawing thread).
- Proposed change: structural sharing instead of copying. `SessionClient` already replaces its room snapshot only when
  one arrives; publish it as `std::shared_ptr<const room::Snapshot>` (badge stripping from `FillRoomView` applied once
  when the pointer changes, keyed by room revision + `classEmblems` + `ServerOwned()`). Do the same for `preferences`,
  `publicRooms`, `rankings`, `pads` (each replaced only where it changes). `RuntimeSnapshot` holds the pointers;
  `ShellView` holds `const room::Snapshot*` plus the elapsed-ms offset instead of a mutated copy (idle seconds and hold
  countdowns at Overlay.cxx:288-300 are computed at draw from `roomReceivedMs`). Memoize the five label strings on
  (lobbyConditions, lobbyMenuCharaID, lobbyStageID, language).
- Behavior preserved: the shell draws the same values; per-tick publication of the pad sample (`menuController`) is
  unchanged, which is why `PublishCadence` publishes every tick in the first place.
- Behavior at risk: none on the wire; old clients unaffected (in-process only). `PresentationSnapshot`'s contract is that
  it owns immutable values the drawing thread can hold across a frame, so sharing requires `SessionClient` to hand out a
  fresh pointer per accepted revision and never mutate a published object. Today it move-assigns `_roomSnapshot` whole
  per accepted snapshot (SessionClient__Room.cxx:397, chat carried over once on `chatUnchanged`), so it can build the new
  `shared_ptr<const room::Snapshot>` there, once per commit instead of once per tick. The idle-seconds/holds adjustment
  must give the same numbers when computed at draw.
- Estimated saving: at menu rate roughly 2-5 MB/s of game-thread memcpy and 10-30k allocations/s removed (two copies x
  60 Hz x ~40 KB / ~250 allocs), bounded by room size; nothing in a quiet room. No change to bytes on any wire.
- Confidence: medium (sizes are from the baseline doc, not measured in-process)

### F-SF4E-6: Pure-sound metadata lives in three pointer-keyed std::maps hit hundreds of times per frame
- File: src/sf4e/sf4e__Game__Battle.cxx:376
- File: src/sf4e/sf4e__Game__Battle.hxx:88
- File: src/sf4e/sf4e__Game__Battle__System__SaveState.cxx:308
- File: src/sf4e/sf4e__Game__Battle__System__SaveState.cxx:813
- Category: judo
- Severity: P2
- Claim: `bUsePureSounds` is on by default. `adapterToCurrentSound` (`std::map<CriPlayerAdapter*, DeferredSoundRequest>`),
  `shadowManagerMap` and `queuedStops` are keyed by engine addresses. `SyncState` runs twice per live frame
  (`PlayGgpoFrame` pre and post step; once per offline frame) and does `adapterToCurrentSound[...]` per adapter several
  times (queued-stop scan x realCount, `PairLiveSounds` lambdas incl. pairwise `IsEqual`, stop loop, reconcile loop);
  every `Save` does a `find` per stub adapter (`criPlayerState.reserve(64)` is the size evidence), every `Load` an
  `operator[]` per adapter through `FindSavedEntry`'s cursor scan, and the engine calls the `IsStillPlaying` detour
  (another lookup) per adapter. That is on the order of 1,000 tree lookups per simulated frame, plus 64 more per
  re-simulated frame. The address keys are also why the destructor must purge entries before the adapters are freed
  ("the next battle's adapters frequently land on these exact addresses", Game__Battle.cxx:200-205) and why
  `ResetAdapterMetadata` exists: a whole bug class created by the container choice.
  Payload, interval, fan-out, transport: in-process; ~128 adapters (64 stub + 64 real), ~1,000 map lookups x
  60 Hz, game thread.
- Proposed change: one `std::vector<ManagerShadow>` with `struct ManagerShadow { rSoundPlayerManager* stub; rSoundPlayerManager*
  real; std::vector<DeferredSoundRequest> stubMeta, realMeta; std::vector<DeferredSoundRequest> queuedStops; }`, metadata
  indexed by adapter index (`adapter - *GetAdapters(manager)`). `Factory` appends, the destructor erases the record (no
  purge loop), `IsStillPlaying` finds its manager by an address-range test over at most ~8 records. `SaveState` records
  per manager `{stub, stubMeta copy, pool}` (a contiguous copy into retained capacity); `FindSavedEntry`,
  `ResetAdapterMetadata` and the per-adapter cursor logic are deleted.
- Behavior preserved: same per-adapter values, same stop/pair/reconcile decisions.
- Behavior at risk: `shadowManagerMap` iteration is address order today; a vector iterates in creation order, which
  changes the order real sounds are stopped/started between managers inside one SyncState (presentation only, never
  simulation). Keep INV-SAVE-ROUNDTRIP (restore exactly what was saved; a manager not recorded is left untouched) and
  INV-PURE-SOUND.
- Estimated saving: ~1,000 tree lookups/frame -> array indexing; ~80 lines deleted (purge, reset, cursor search); one
  latent stale-address bug class removed.
- Confidence: medium

### F-SF4E-7: Every save frees and re-allocates ~90 std::set nodes in `trackedKeys`
- File: src/sf4e/sf4e__Game.cxx:43
- File: src/sf4e/sf4e__Game.cxx:51
- File: src/sf4e/sf4e__Game__Battle__System__SaveState.cxx:790
- File: src/sf4e/sf4e__MementoGuards.cxx:95
- Category: overhead
- Severity: P2
- Claim: `SaveState::Save` zeroes every live key after copying it (SaveState.cxx:799), so the next frame's record finds
  `mementoableObject == 0` and calls `GameMementoKey::Initialize` for each of the ~90 keys (MementoGuards.hxx:49-50,
  SAVESTATE_FREE.md: Initialize calls ClearKey). Both are detoured: the native Initialize's internal call to ClearKey
  (0x52F3D0) lands in `fKey::ClearKey`, which does `trackedKeys.erase(this)` (frees a node), and `fKey::Initialize` then
  does `trackedKeys.insert(this)` (allocates one). `trackedKeys` is a `std::set<rKey*>`. That is ~90 node free + ~90 node
  malloc per save, and a save happens once per simulated frame and once per re-simulated frame. On top, `Load` does ~270
  `std::set::find` calls (`CheckKeyWrite` in three passes) and `FreeDetached` ~90 more.
  Payload, interval, fan-out, transport: in-process; ~180 heap ops per save, i.e. ~10.8k/s at 60 Hz with no
  rollback, x(1 + rollback depth) during rollbacks; game thread, inside GGPO callbacks.
- Proposed change: (minimal) a file-static `inInitialize` depth in `fKey::Initialize` so the nested `fKey::ClearKey` keeps
  the key tracked (`NoteEngineClear` still runs); membership after Initialize is identical. (Better) replace the
  `std::set` with a flat container that never reallocates in steady state, e.g. a sorted `std::vector<rKey*>` or the
  same open-addressing scheme `PayloadOwners` already uses, so `CheckKeyWrite` becomes a cache-friendly probe.
- Behavior preserved: the tracked set's contents after every engine call; the save copies keys in the same (address)
  order with a sorted vector.
- Behavior at risk: none if the minimal version is used. A hash container changes `keys` order inside a SaveState; keys
  are installed by address so restore is order-independent, but keep address order to be safe (INV-SAVE-ROUNDTRIP,
  INV-MAIN-THREAD).
- Estimated saving: ~180 allocator calls per simulated frame (more in rollbacks); ~360 tree walks per load become probes.
- Confidence: medium (the Initialize-calls-ClearKey routing is from SAVESTATE_FREE.md and the Detours entry patch; verify
  with a counter on `trackedKeys` inserts per save before changing)

### F-SF4E-8: Discord presence JSON is rebuilt and re-sent every second, with a full room copy, as a keepalive
- File: src/sf4e/sf4e__NetplayRuntime__Status.cxx:196
- File: src/discord/Presence.hxx:9
- File: src/discord/main.cxx:66
- Category: streaming
- Severity: P2
- Claim: `PublishDiscordPresence` already keys the presence on an 8-word `discordPresenceKey`, but `publishDue = now -
  discordLastPublish >= 1000` makes it rebuild `PresenceInput` (which takes `room::Snapshot room` by value, so a full room
  copy incl. chat), run `discord::Describe`, build an `nlohmann::json` and `dump()` it, and send it over the helper pipe
  every second even when nothing changed. The companion needs a message at least every 15 s or it exits
  (`GetTickCount64()-lastInput>15000`, main.cxx:66) and dedups unchanged presence itself, so the per-second resend is a
  heartbeat carrying a full payload.
  Payload, interval, fan-out, transport: `{"type":"presence",...}` ~200-300 B JSON, 1 Hz, fan-out 1, named
  pipe to `sf4-discord`; plus a ~35-40 KB in-memory room copy per send.
- Proposed change: on `key` unchanged, resend the cached `runtime->discordPublished` string without rebuilding (no room
  copy, no Describe, no JSON), and stretch the heartbeat to 5 s (well under the companion's 15 s). Change
  `PresenceInput::room` to `const room::Snapshot*`. Do NOT introduce a new message type: the companion treats any
  non-`presence` type as fatal (`running=false`), and an older companion could be paired with a newer game during an
  update.
- Behavior preserved: same presence content; the companion still sees input well inside 15 s; secret expiry is still
  enforced by the companion itself (main.cxx clears an expired secret before sending).
- Behavior at risk: a secret that expires while the key is unchanged is no longer cleared by `Describe` on the game side
  until the key changes; the companion already clears it, so Discord's view is the same.
- Estimated saving: ~0.25 KB/s -> ~0.05 KB/s on the pipe; ~40 KB/s of room copying and one JSON build/s on the game thread
  removed. Small, but it is exactly the "resent on a timer whether or not it changed" shape the owner asked for. (The only
  15 s timer in this area is `kNetplayWindowMs`, Ggpo.cxx:69: one ~250 B log line per 15 s, not a JSON send. No per-frame
  JSON or per-frame logging exists in the rollback hooks.)
- Confidence: high

### F-SF4E-9: Main-menu ticks re-read devices, settings and pads from scratch
- File: src/sf4e/sf4e__NetplayRuntime__Input.cxx:80
- File: src/sf4e/sf4e__NetplayRuntime__Input.cxx:122
- File: src/sf4e/sf4e__InputDevices.cxx:88
- File: src/sf4e/sf4e__OverlayPrefs.cxx:213
- Category: overhead
- Severity: P2
- Claim: `CaptureMenuInput` runs every tick. At the main menu it calls `input::ReadDevices()`, which loops
  `GetAsyncKeyState` over 248 virtual keys (InputDevices.cxx:88) to set one `keyboard.buttons` bit, builds a vector with
  name strings, and `LogInventory` builds a concatenated key string to compare. Then, in every state, it does
  `OverlayPrefs::Data prefs; OverlayPrefs::Load(prefs);` only to read `prefs.deviceIdentity`: `Load` takes a mutex shared
  with the drawing thread and copies the whole `Data` (44 per-fighter lobby records, random pool, identity strings); on a
  cache miss (no valid `overlay` object in settings.json yet) it falls through to `SettingsStore::Access`, which creates
  the directory, opens `settings.lock` and parses the whole settings.json on the game thread, every tick, until something
  saves. Finally `pads` is rebuilt with `ui::LayoutButtonNames` strings per pad per tick.
  Payload, interval, fan-out, transport: in-process; 248 syscalls + ~1-2 KB copy + ~10-30 string allocations per
  tick at 60 Hz (main menu); worst case one settings.json read+parse per tick on a cache miss.
- Proposed change: add `OverlayPrefs::DeviceIdentity()` returning only the optional identity under the lock; remember a
  failed load (negative cache cleared by Save/StartPersistence) so a miss does not hit the disk per tick. Scan the
  keyboard only when the keyboard bit has a reader (capture not Idle, or the Controllers page shown via a
  `LatencyBoardPulse`-style note; `ControllerInventory` reads `buttons` for the pressed indicator). Rebuild `pads` only
  when `padInventory.Build`'s inputs change (layer revision, device list key already computed by `LogInventory`).
- Behavior preserved: same device choice, same capture outcomes, same pressed indicator when the page is shown.
- Behavior at risk: a settings.json that gains an `overlay` section from another process while the game runs is picked
  up at the next save instead of the next tick (the launcher is the only other writer).
- Estimated saving: ~15k syscalls/s and a few thousand allocations/s at the main menu; removes a potential per-tick disk
  read. Menu only; no effect in fights.
- Confidence: medium

### F-SF4E-10: `Runtime` is a 120-field god object whose match lifecycle is reset field by field
- File: src/sf4e/sf4e__NetplayRuntime.hxx:112
- File: src/sf4e/sf4e__NetplayRuntime.cxx:30
- File: src/sf4e/sf4e__NetplayRuntime.cxx:93
- Category: structural
- Severity: P2
- Claim: `Runtime` holds ~120 members mutated from eight translation units. The match-entry and teardown lifecycle alone
  is ~25 loose fields (`matchEntered`, `enteredGeneration`, `enteredTable`, `sessionlessCloseGeneration`,
  `spectatorStreamFailedGeneration`, `entryDeferredGeneration/SinceMs`, `matchEnded`, `matchFinishedPending/Generation/
  Table/Action`, `finishActionId/RetryAt`, `terminalAckPending/OutcomeConsumed/Table/Generation`, `committedEndGeneration`,
  `pendingAbort/Deadline`, `recoveringMatch`, `participantLeftGeneration`, `matchEndLog`, `matchInput*`). `CloseRoom`
  resets about 30 of them by hand and `ResetMatchEntry` resets a different subset; which fields belong to "room", "match
  entry" and "terminal receipt" is only knowable by reading both. A field added to the struct and forgotten in
  `CloseRoom` survives into the next room, the half-reset the skill warns about.
- Proposed change: group into value sub-structs with default member initializers: `MatchEntry` (entered*, deferred*,
  sessionless/streamFailed generations), `MatchTeardown` (finished*, terminal*, pendingAbort*, committedEnd, endLog),
  `RoomLeave` (leave*, recovery*, join kind), `MatchInputBinding`. `ResetMatchEntry()` becomes `runtime->entry = {};
  nextRequest = {};`, `CloseRoom` becomes a handful of `= {}` plus the calls that have side effects.
- Behavior preserved: identical reset sets (mechanical move).
- Behavior at risk: none identified; check that no field reset in only one of the two functions moves to a struct reset by
  the other.
- Estimated saving: ~40 lines; reset becomes atomic per concept.
- Confidence: high

### F-SF4E-11: `StartGGPO` and `StartSpectating` duplicate the session reset, and split failure handling with their caller
- File: src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:376
- File: src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:494
- File: src/sf4e/sf4e__UserApp.cxx:269
- Category: spaghetti
- Severity: P2
- Claim: both functions repeat ~25 lines in slightly different orders: `rollbackHud.Reset`, `FixBattleStepForMatch`,
  `diag::InitFromEnvironment`, `CancelDeferredGgpoClose`, leftover `RetireGgpoSession`, `matchTelemetry.Reset`,
  `diag::ResetForMatch`, `ResetNativeResultMatch`, `ClearMatchNotice`, the `Reclaim` loop, `simGate.OnSessionStarted`,
  `ResetPacerForSession`, latch/abort/disconnect resets, `s_streamWatch.Start`, players reset, `ggpoTimeline.Reset`, the
  seven-line `GGPOSessionCallbacks` block, and `nNextBattleStartFlowTarget`/`bTerminateOnNextLeftBattle`. Already
  diverged: `StartSpectating` calls `matchTelemetry.Reset(true)` after the leftover retire while `StartGGPO` calls it
  before, and `StartSpectating` does not call `LogSaveStateFreePolicy`. Failure handling is split: `StartGGPO` aborts
  itself on `ggpo_start_session`/`add_player` failure, `StartSpectating` only logs and relies on
  `_OnVsBattleTasksRegistered` (UserApp.cxx:269) to abort when `ggpo` is null. For a fighter both fire, and the caller's
  generic `runtime.authorized_connection_unavailable` replaces the specific `runtime.ggpo_start_failed` notice
  (`MatchNoticeState::Push` overwrites).
- Proposed change: `static void BeginSession(const char* label, bool spectator)` holding the shared reset in one order,
  and `static GGPOSessionCallbacks Callbacks()`. Let the two `Start*` functions return `bool` and never abort; the single
  caller aborts once with the most specific reason it was given.
- Behavior preserved: same state after a successful start.
- Behavior at risk: the order of `matchTelemetry.Reset` vs. the leftover retire must be chosen once (retire first is the
  safer order; `RetireGgpoSession` also resets telemetry). Failure notices become the specific ones (intended).
- Estimated saving: ~35 lines; one session-start sequence.
- Confidence: high

### F-SF4E-12: Decomposition seams for the four files near 900-1000 lines
- File: src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx:1
- File: src/sf4e/sf4e__Game__Battle__System.cxx:1
- File: src/sf4e/sf4e__Game__Battle__System__SaveState.cxx:1
- File: src/sf4e/sf4e__Overlay.cxx:470
- Category: size
- Severity: P2
- Claim: each file mixes three or four concerns that do not share state.
  - Ggpo.cxx (976): pacing + `NetplayWindow` 15 s telemetry (17-75, 168-234, 585-619, ~200 lines); spectator policy and
    links (96-141, 833-849 plus event-callback branches); session lifecycle (236-555); callbacks (621-976).
  - System.cxx (949): native match result timeline (13-107); System memento detours and task-handle rebinding
    (159-332) plus `RecordAll/RestoreAllFromInternalMementos` and `MementoUnitTimer` (779-949), together ~350 lines that
    belong with `SaveState`; the frame loop (335-584) and battle open/close.
  - SaveState.cxx (878): its own first line names three concerns. Semantic hash, checkpoint ring and v1 snapshot capture
    (55-303) is a clean cut; release policy (detached, round trip, verify hash, 469-645) is another. `CopyIntoPlace` and
    `Clear` are non-static global symbols (SaveState.cxx:321, 397) and should be file-local or members.
  - Overlay.cxx (881): `DrawOverlay` is a ~340-line function holding 12 function-static "seen" latches
    (`inviteShown`, `joinLinkShown`, `trainingCallShown`, `connectLinkShown`, `replayLinkShown`, `roomLinkShown`,
    `logOpensSeen`, `returnsSeen`, `controlsWereOpen`, `takenSeen`, `refusedSeen`, `wasCapturing`...). Five of them
    (503-540) are the same rule "open the menu once per new sequence when at main menu and available"; they become a
    table of `{sequence getter, extra condition, reason}` processed in one loop. Seams: ShellView building (276-467) to
    `OverlayShellView.cxx`; training and replay keys (600-681) to `OverlayTrainingKeys.cxx`; window procedure.
- Proposed change: split along those seams into `..._Pacing.cxx`, `..._Spectators.cxx`, `..._NativeResult.cxx`,
  `..._Memento.cxx`, `..._StateHash.cxx`, `..._Release.cxx`, and the two overlay files; replace the five open latches with
  a `MenuOpenTriggers` table.
- Behavior preserved: pure moves except the trigger table, which preserves each trigger's condition and order.
- Behavior at risk: none (no logic change); keep every new TU including `System__Internal.hxx` so the aliases stay private.
- Estimated saving: no file over ~500 lines; ~30 lines from the trigger table; 12 static latches -> 7.
- Confidence: high

### F-SF4E-13: Battle-flow globals and fighter getters are enumerated by hand in five places
- File: src/sf4e/sf4e__Game__Battle__System__SaveState.cxx:863
- File: src/sf4e/sf4e__Game__Battle__System__SaveState.cxx:329
- File: src/sf4e/sf4e__Game__Battle__System__SaveState.cxx:425
- File: src/sf4e/sf4e__Game__Battle__System__SaveState.cxx:245
- File: src/sf4e/sf4e__Game__Battle__System__SaveState.cxx:71
- Category: judo
- Severity: P3
- Claim: the ten battle-flow globals are listed field by field in `Save` (863-872), `CopyIntoPlace` (329-338), `Clear`
  (425-434) and partly in `ComputeSemanticHashes`/`HashLiveStateForFreeCheck`; a global added to one list and not the
  others breaks INV-SAVE-ROUNDTRIP silently. They are two contiguous engine blocks (0x664994..0x6649A3 and
  0x6A9254..0x6A926B). Separately, `CaptureSnapshot` (v1) re-reads 18 of the 24 getters `CaptureCharaSemantics` (v2)
  already reads for the same frame; two readers of one model.
- Proposed change: `struct BattleFlowGlobals { ...; static BattleFlowGlobals Capture(); void Restore() const; }` used by
  Save/CopyIntoPlace; `Clear` does `victim->d.flow = {}`. Build `StateSnapshot` fields from the frame's `CharaSemantics`
  (FrameTrace already keeps the row per state frame) instead of a second getter pass.
- Behavior preserved: same bytes saved and restored; same snapshot values (the snapshot is captured after the corrected
  frame either way, since a re-simulated frame replaces its trace row).
- Behavior at risk: v1 snapshot values must stay bit-identical (it is compared with `memcmp` on the peer); verify the
  FixedPoint word packing matches `StateSnapshot`'s layout before switching.
- Estimated saving: ~35 lines; one list of globals; 36 getter calls per second (negligible CPU). Separately, as a decision
  and not a no-behavior change: v1 `battle_snapshot` JSON (once per second per player, forwarded to every room member)
  carries a strict subset of what v2 `battle_hash` already proves every 30 frames; DESYNC_V2.md keeps v1 until v2 is
  proven, after which retiring v1 removes that per-second JSON per fighter x room fan-out (sent by session-area code).
- Confidence: medium

### F-SF4E-14: Localized strings are looked up (and allocated) every tick for messages that are almost never shown
- File: src/sf4e/sf4e__UserApp.cxx:427
- File: src/sf4e/sf4e__NetplayRuntime__Status.cxx:538
- File: src/sf4e/sf4e__NetplayRuntime__Status.cxx:184
- File: src/common/Localization.cxx:181
- Category: overhead
- Severity: P3
- Claim: `Steam_PostUpdate` evaluates `sf4e::loc::T(netplayStepFailed ? "runtime.room_link_lost" :
  "runtime.session_server_error")` every tick for `ObserveControlPlane`, which uses it only on the loss edge. `Publish`
  calls `loc::T` for `discordStatus`, every lock reason and room error per publish. `loc::T` does
  `unordered_map<std::string, std::string>::find(const char*)`, which constructs a `std::string` key per call; ids are
  usually longer than the 15-char SSO buffer, so each lookup allocates.
  Payload, interval, fan-out, transport: in-process; a few allocations + hashes per tick, 60 Hz.
- Proposed change: pass the catalog id to `ObserveControlPlane`/`HandleNetplayFailure` and resolve on the edge; make
  `Catalog` use a transparent hash (`std::string_view` lookup) so `T` never allocates.
- Behavior preserved: same texts.
- Behavior at risk: none identified.
- Estimated saving: a few hundred allocations/s; trivial CPU, but it removes an allocation from every outer tick.
- Confidence: medium

### F-SF4E-15: Dimps exposes ~130 struct-offset accessors as out-of-line functions with no LTCG
- File: src/Dimps/Dimps__Game__Battle__System.cxx:61
- File: src/Dimps/Dimps__Game__Battle__Chara.cxx:1
- Category: size
- Severity: P3
- Claim: accessors like `FixedPoint* System::GetNumFramesSimulated_FixedPoint(System* s) { return (FixedPoint*)((unsigned
  int)s + 0xdd4); }` are defined in .cxx files (~130 across Dimps, ~4 lines each) and CMake configures no
  INTERPROCEDURAL_OPTIMIZATION, /GL or /LTCG (checked CMakeLists.txt, CMakePresets.json and cmake/), so every hot-path read (several per simulated frame) is a cross-TU call and every new
  field costs a declaration plus a definition.
- Proposed change: one inline helper in a Dimps header, `template <class T, std::size_t Offset, class Base> T* Field(Base*
  b) { return reinterpret_cast<T*>(reinterpret_cast<char*>(b) + Offset); }`, and the accessors become inline one-liners in
  the headers (or `static constexpr` offsets).
- Behavior preserved: identical addresses.
- Behavior at risk: none.
- Estimated saving: ~400 lines; calls in the frame loop become inlined loads.
- Confidence: high

### F-SF4E-16: Small per-frame copies on the overlay and presentation path
- File: src/sf4e/sf4e__Overlay.cxx:798
- File: src/sf4e/sf4e__NetplayPresentation.cxx:102
- Category: overhead
- Severity: P3
- Claim: every drawn frame copies `s_prefs` (`OverlayPrefs::Data`, 44 per-fighter records plus strings) into a local,
  patches four fields and compares the whole struct to detect an edit (Overlay.cxx:798-803). Every outer tick
  `PublishPresentationFrame` allocates a new `PresentationSnapshot` and `GetStatus()` walks the room's members and
  string-compares lobby member names to find the opponent, whether or not anything changed.
- Proposed change: mark prefs dirty where `lobbyConditions`/`lobbyStageID`/`lobbyRandomPool` are edited (the selector
  callback and the `selectionStep` branch) and save then; publish the presentation envelope only when `GetStatus()`'s
  inputs or `runtime` pointer changed (compare the sequence of the runtime snapshot plus a small status fingerprint).
- Behavior preserved: same saves, same HUD values.
- Behavior at risk: the countdown fields (`disconnectCountdownMs`, notice expiry) change every tick while active, so the
  fingerprint must include them, i.e. publish every tick while a warning is active.
- Estimated saving: ~2 KB copy + compare per draw frame; one allocation per tick.
- Confidence: medium

## Area summary
- Health: the rollback core is careful and well documented (gate model, abort latch, allocation-free save records,
  detached release, bounded rings); no per-frame JSON, logging or string formatting exists in the battle hooks, and
  diagnostics are gated. The debt is structural: duplicated deterministic steps, mode predicates scattered across layers,
  a 120-field runtime object and a 420-line dispatcher.
- Biggest judo move: F-SF4E-1 + F-SF4E-3 together. One `SimulateWithInputs` core shared by live, resim and orphan frames,
  plus a session role and a drain phase owned by `GgpoGateModel`, delete the hand-kept copies and ~25 ad-hoc predicates
  and take the facade out of the GGPO callbacks.
- Biggest overhead item: menu-rate deep copies of the room (F-SF4E-5: two ~40 KB copies per frame in a busy room), then the
  per-frame container churn in the rollback path (F-SF4E-7 std::set node churn per save, F-SF4E-6 ~1,000 sound-map lookups
  per frame). The only recurring send in this area is the 1 s Discord presence keepalive (F-SF4E-8); the 15 s item here is
  a log line, not JSON.
- Savestate size and copies per frame: one Free (detached, ~90 engine ClearKey) and one Save (~90 engine Initialize +
  record, 90 x 48 B key copies, ~64 sound records, GameManager memcpy, replay recorder capture) per simulated frame, x(1 +
  depth) in rollbacks. The engine-side alloc/free of every memento buffer per frame is inherent to the ownership-transfer
  design; recycling the victim's buffers into the live keys would remove it but writes into live keys, which v1.0
  deliberately stopped doing (SAVESTATE_FREE.md). Not proposed.
- Not assessed: GGPO fork internals; real timings (no diagnostics run, all costs estimated from code); whether
  `SyncState`'s second real-manager `Update` per live frame is intended; `InputLagControl::BeforeFrame` (appears
  edit-on-change); the remaining NetplayRuntime phase files (Match, Tournament, PublicRooms, Rankings) were skimmed only.
