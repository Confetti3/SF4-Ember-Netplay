# GGPO integration lifecycle audit (Phase 0)

Baseline: branch `main` @ `a6a3aa9fa0de40aa` ("Fix training room round timer instantly
expiring"), clean working tree (untracked `crash-logs-20260718/` only). Build preset
`default` (= `x86-msvc-ninja-relwithdebinfo`) builds clean; `ctest` baseline:
`GgpoUdpValidation` **PASSED** (the only automated test). `SessionInteractiveTest`
and the two-machine matrix in `docs/TRANSPORT_REGRESSION.md` are live-test-only.

Pinned GGPO: `adanducci/ggpo@c88b667` via `vcpkg-ports/ggpo` with
`install-cmake-export.patch` + `reject-invalid-udp-messages.patch`. All fork claims
below were verified against the extracted patched source in the local vcpkg
buildtree (`buildtrees/ggpo/src/c88b667-*.clean`).

## Thread affinity

**Everything below runs on the game's main thread.** There are no worker threads in
the netplay integration:

- `fUserApp::Steam_PostUpdate` is a detour of the engine's per-tick Steam update.
- GameNetworkingSockets is used in callback-pump mode: `RunCallbacks()` is called
  from `Steam_PostUpdate`, so `SessionClient::OnSteamNetConnectionStatusChanged`
  runs on the main thread.
- Every GGPO entry point (`ggpo_add_local_input`, `ggpo_synchronize_input`,
  `ggpo_advance_frame`, `ggpo_idle`, `ggpo_close_session`) is called from either
  `fSystem::BattleUpdate` (detour, main thread) or `Steam_PostUpdate`.
  GGPO's fork has no internal threads; all callbacks (`save/load/free/advance/
  on_event`) are invoked synchronously from those entry points, therefore also on
  the main thread. `SaveState::Save/Load/Free` are main-thread-only and must stay
  that way (engine memento calls mutate live engine objects).

## Outer tick (one rendered frame)

```
Engine tick
├─ fSystem::BattleUpdate (detour)                  [only while a battle exists]
│  ├─ gate: if (!bUpdateAllowed) return
│  ├─ if (ggpo && flow != BF__IDLE):               [netplay path]
│  │  ├─ ggpo_add_local_input(local inputs)
│  │  │    └─ may return PREDICTION_THRESHOLD → frame silently skipped
│  │  ├─ ggpo_synchronize_input → playbackData
│  │  ├─ SoundPlayerManager::SyncState()           [pure sounds pre-step]
│  │  ├─ original rSystem::BattleUpdate            [deterministic sim step]
│  │  ├─ ggpo_advance_frame
│  │  │    └─ Sync::IncrementFrame → SaveCurrentFrame → save callback
│  │  ├─ SyncState() again (post-step) + CaptureSnapshot (every 60 frames)
│  │  └─ any failure → AbortGgpoMatch
│  └─ else: SyncState() + original BattleUpdate    [offline/training path]
├─ ... engine renders ...
└─ fUserApp::Steam_PostUpdate (detour)
   ├─ NetplayFacade::TickMainMenu (auto-start state machine)
   ├─ SteamNetworkingSockets()->RunCallbacks()
   ├─ SessionClient::Step   (room JSON messages; snapshot send/compare)
   ├─ SessionServer::Step   (host only)
   ├─ step failure → HandleNetplayFailure(closeGgpo=true)  ← control-plane
   │                                                          loss kills GGPO
   ├─ NetplayFacade::TickFrame (pending match start, lobby settings,
   │    GGPO sync watchdog, deferred GGPO close, GgpoRelay::Pump)
   ├─ if (ggpo) ggpo_idle(ggpo, 1)                 ← see "ggpo_idle" below
   └─ original Steam_PostUpdate
```

## Rollback resimulation

`Peer2PeerBackend::DoPoll` (called from `ggpo_idle` and from `ggpo_add_local_input`
via `DoPoll(0)`) runs `Sync::CheckSimulation` when input from the remote proves a
prediction wrong:

```
Sync::AdjustSimulation(seek_to)
├─ LoadFrame(seek_to)          → load callback → SaveState::Load
└─ for count frames:
   └─ advance_frame callback   → fSystem::ggpo_advance_frame_callback
      ├─ ggpo_synchronize_input
      ├─ original (undetoured) rSystem::BattleUpdate   ← INVARIANT
      ├─ ggpo_advance_frame → save callback (per resim frame)
      └─ CaptureSnapshot
```

Notes:
- The rollback callback intentionally does **not** run `SyncState()` per resimulated
  frame; pure-sound reconciliation happens around the *outer* simulation step, and
  stale queued stops are cleared inside `SaveState::Load` (timeline abandonment).
- `fPadSystem::playbackFrame` is set/restored around both the normal and rollback
  paths; there is currently no scope guard, so an early return between set and
  restore would leak playback mode (Phase 2 hardens this).
- The advance-frame callback ignores its frame argument; the save callback ignores
  its frame argument (Phase 6 will store it). Exact rollback origin/destination is
  **not observable** through the callback interface — only the consecutive
  rollback-callback burst length is (a proxy).

## Save/load/free state ownership

- `saveStates[NUM_SAVE_STATES]` (= `GGPO_MAX_PREDICTION_FRAMES + 2` = 10) is a
  preallocated pool. GGPO's ring buffer stores pointers into this pool; the save
  callback reports `*len = 1` (nonzero-length workaround, GGPO asserts otherwise)
  and never transfers ownership.
- `SaveState::Save`: `RecordAllToInternalMementos(GGPO_MEMENTO_ID)` (engine-owned
  memento payloads land behind `fKey::trackedKeys`), then each tracked key is
  copied into `dst->keys` and the live key zeroed (otherwise engine
  reinitialization would free the saved payload). Sound adapter metadata and
  pool state are copied into pointer-keyed maps; battle-flow globals and the
  `GameManager` block are memcpy'd (shallow — contains pointers).
- `SaveState::Load`: backs up all currently-tracked live keys into a temporary
  vector (fresh allocation per load), installs the source state's keys
  (`CopyIntoPlace` → `RestoreAllFromInternalMementos`), zeroes injected keys,
  restores the live keys. Also clears `queuedStops` (abandoned timeline).
- `SaveState::Free`: saves the *live* timeline into a fresh temporary `SaveState`,
  installs the victim so each `GameMementoKey`'s mementoable pointer is valid,
  `ClearKey`s the victim's keys (delegates to the engine object — this is why the
  victim must be installed), then restores the live timeline. Two full
  `CopyIntoPlace` round-trips + one full `Save` per freed state, all on the main
  thread, inside GGPO's `free_buffer` callback (which fires during
  `SetLastConfirmedFrame` retirement and slot reuse).

## `ggpo_idle` (fork evidence, Phase 3 input)

`ggpo_idle(session, timeout)` → `Peer2PeerBackend::DoPoll(timeout)`:
- `_poll.Pump(0)` — nonblocking (`WaitForMultipleObjects` timeout 0; the UDP
  socket is nonblocking (`FIONBIO`) and registered as a *loop* sink, so reads
  happen in `OnLoopPoll` regardless of the wait).
- After the pump, when not synchronizing and not in rollback:
  `if (timeout) { Sleep(1); }` — annotated `// XXX: this is obviously a farce...`
  in the fork. So the current `ggpo_idle(ggpo, 1)` in `Steam_PostUpdate` is an
  **unconditional Sleep(1) every outer tick during a match** — 1–15.6 ms
  depending on timer resolution. `ggpo_idle(ggpo, 0)` performs the identical
  pump work without the sleep.

## Time-sync (fork evidence, Phase 4 input)

- `TimeSync::recommend_frame_wait_duration` returns a **fresh estimate** of how
  many frames we are ahead (half the averaged local/remote advantage delta),
  already clamped to `MAX_FRAME_ADVANTAGE` = 9 and gated at
  `MIN_FRAME_ADVANTAGE` = 3. Recommendations are *not* additive deltas; a new
  event supersedes the previous estimate (correct pacing policy: replace/max,
  never sum).
- Events fire at most once per `RECOMMENDATION_INTERVAL` = 240 frames (~4 s).
- Current handler: `Sleep(1000 * frames_ahead / 60)` inside the `on_event`
  callback — a 50–150 ms hard stall in the middle of `DoPoll`, i.e. inside the
  outer frame.

## GGPO error codes reachable from our call sites (Phase 2 input)

From the fork: `AddLocalInput` → `NOT_SYNCHRONIZED` (during sync/rollback... via
`_synchronizing`), `PREDICTION_THRESHOLD`, `INVALID_PLAYER_HANDLE`/
`PLAYER_OUT_OF_RANGE` (bad handle), `IN_ROLLBACK`; `SyncInput` →
`NOT_SYNCHRONIZED`; `AdvanceFrame`/`Idle` → `GGPO_OK` on the p2p backend.
`DISCONNECTED_FROM_PEER` events (not error codes) signal terminal disconnect.

## All writers of `fSystem::bUpdateAllowed`

| Site | Value | Meaning |
|---|---|---|
| static init (`Battle__System.cxx:73`) | true | default (offline play works) |
| `BattleUpdate` (`bHaltAfterNext`) | false | debug single-step halt |
| `AbortGgpoMatch` | false | terminal failure |
| `StartGGPO` / `StartSpectating` | false | gate until RUNNING |
| `ggpo_on_event_callback` RUNNING | true | sync complete |
| `ggpo_on_event_callback` CONNECTION_INTERRUPTED | false (saves prior into `s_updateAllowedBeforeGgpoInterrupt`) | temp warning — freezes sim |
| `ggpo_on_event_callback` CONNECTION_RESUMED | restores saved bool | unsafe if another gate reason changed it meanwhile |
| `NetplayFacade::HandleNetplayFailure` | false | room failure |
| `fUserApp::TryRestartGgpoLegacyTunnel` | false | legacy tunnel restart |
| Overlay dev panel (`Overlay.cxx:1940–1950`) | true/false | manual pause / step |

This single boolean currently conflates: startup gating, debug pause, connection
warning, terminal failure, and room failure. Phase 2 replaces it with an explicit
gate model.

## All sites that close or null the GGPO session

- `fSystem::CloseBattle` (unless spectator defer)
- `fSystem::AbortGgpoMatch`
- `fSystem::StartGGPO` (leftover session before rematch/restart)
- `fUserApp::_OnVsBattleTasksRegistered` (before UDP registration rebind)
- `fUserApp::TryRestartGgpoLegacyTunnel`
- `NetplayFacade::TickFrame` (deferred spectator close)
- `NetplayFacade::ShutdownNetplay(closeGgpo=true)` — reached from
  `HandleNetplayFailure`, i.e. **room/control-plane loss closes a healthy GGPO
  session today** (Phase 7 target). Also `SessionClient` connection-state change
  while in-match triggers the same path.

## All sites that clear battle snapshots / sound state

- `NetplayFacade::ClearBattleState` → `snapshotMap.clear()` +
  `pendingRemoteSnapshots.clear()`; called from `StartMatchFromLobby`,
  `AbortGgpoMatch`, `NotifyMatchEnded` (i.e. `CloseBattle`).
- `SessionClient::Step` erases individual snapshots once sent+confirmed.
- Sound: `fSoundPlayerManager::destructor` purges adapter metadata + queued stops
  + shadow map for the dying manager (cross-battle safety); `Factory` resets
  adapter metadata on reuse; `SaveState::Load` clears `queuedStops` only.

## Snapshot (“desync detection v1”) semantics

`CaptureSnapshot` records selected character state every 60 simulated frames
(also from rollback resim — re-captures overwrite by frame index). Snapshots are
sent by `SessionClient::Step` only once they are 60+ frames old (proxy for
"non-speculative"; **not** GGPO-confirmed). Comparison is raw `memcmp` of the
reconstructed struct; mismatch sets `RS_ISLEAVING` (ends the match) on whichever
peer detects it. `StateSnapshotMeta.confirmed` means "peer compared", not GGPO
frame confirmation. The room server forwards snapshots to **all** other clients,
including spectators; a spectator currently cannot send snapshots
(`_snapshotsEnabled` is only set on the joining path — both players and
spectators connect the same way, so spectator mismatch policy must be addressed
in Phase 6).

## Match close / rematch / spectator defer

`CloseBattle` → `NotifyMatchEnded` (decides spectator defer *before* close) →
close session unless deferred (120 s window) → free all used save states →
original `CloseBattle`. Rematch: lobby `LobbyReset` → new ready cycle → new
`StartGGPO` (closes any leftover session first, `CancelDeferredGgpoClose`).
Training rooms are ordinary VS battles with rounds=99 / huge time limit.
