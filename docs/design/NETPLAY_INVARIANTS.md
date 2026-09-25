# sf4e netplay invariants

Do not change these behaviors without regression testing (SessionInteractiveTest, LAN 2P, relay 2P).

## GGPO (`sf4e__Game__Battle__System.cxx`)

- `ggpo_start_session` / `ggpo_start_spectating` use the same callback set: `begin_game`, `advance_frame`, `save_game_state`, `load_game_state`, `free_buffer`, `on_event`, `log_game_state`.
- `ggpo_advance_frame_callback` calls **undetoured** `rSystem::BattleUpdate`, never `fSystem::BattleUpdate`.
- Savestates use the preallocated `saveStates[]` pool; `ggpo_save_game_state_callback` sets `*len = 1`; `ggpo_free_buffer` releases slots.
- Local input: `ggpo_add_local_input` in `fSystem::BattleUpdate`; rollback uses `ggpo_synchronize_input` and `fPadSystem::playbackData`.
- When `bUsePureSounds` is enabled, call `fSoundPlayerManager::SyncState()` around simulation steps.
- Create the GGPO session in `fUserApp::_OnVsBattleTasksRegistered` (after VsBattle load), not earlier.
- Fighter-link disconnect tolerance: **8000 ms / notify 1500 ms**, owned by `common/GgpoDisconnectTolerance.hxx` and applied in `StartGGPO` only (the spectator client backend rejects it). It must stay under iroh's 15 s direct-path idle and openraft's 10-12 s election. The launcher payload no longer carries it. Regression gate: `GgpoOutageTolerance` (a 6.5 s silence resumes; a longer one disconnects at about 8 s).

## Session protocol (`sf4e__SessionProtocol.hxx`)

- Preserve JSON message `type` strings and field names for lobby, prebattle, battle sync, snapshots, and punch signaling (`punch_ready`, `punch_go`).
- Keep `sidecarHash` join validation (`JR_HASH_INVALID`).
- Desync detection via `StateSnapshot` exchange must remain enabled in normal play.

## Rollback remediation (2026-07, feat/rollback-diagnostics-and-pacing)

- `bUpdateAllowed` carries only the manual or developer pause. Session
  lifecycle, terminal failure and room failure live in `GgpoGateModel`, and
  nothing in the session lifecycle writes `bUpdateAllowed`; the host asks
  `fSystem::MayAdvanceDeterministicFrame()`, which is `simGate.MayAdvance`.
  Connection warnings must NOT freeze simulation; prediction stalls are enforced
  by GGPO refusing local input (`GgpoGateModel`, `ClassifyGgpoResult`).
- An online match is a native offline Versus whose pad reads are replaced only
  while a session exists. Every netplay battle is claimed in
  `fUserApp::_OnVsBattleTasksRegistered`, before its session starts, and stays
  claimed until `CloseBattle`. A claimed battle whose session is retired for
  any reason is orphaned (`NativeExitRequired`): `BattleUpdate` drives it with
  neutral playback input for both slots and re-asserts `RS_ISLEAVING` on every
  update until the engine closes it. Never let a netplay battle run the native
  pad path (F-016), and never write `RS_ISLEAVING` from a retirement path: the
  orphan handling is the one place that leaves a battle.
- A `CONNECTION_RESUMED` event clears only the warning; it can never undo a
  manual pause, a fatal abort, or the startup gate.
- Routine in-match polling is `ggpo_idle(session, 0)` (nonblocking; the fork's
  nonzero timeout is an unconditional Sleep). Do not reintroduce a sleep there.
- The timesync event callback must not block; corrections flow through
  `PacingController` (bounded, replace-not-sum, reset on session lifecycle) and
  are repaid ≤ `maxStepMs` per outer frame, never by skipping/doubling
  deterministic simulation.
- SaveState Save/Load/Free stay on the game main thread (debug-asserted).
- Desync v2 (`battle_hash`) is diagnostics-first: no default termination,
  spectator mismatches never end the players' fight, and the legacy
  `battle_snapshot` system stays active (see docs/design/DESYNC_V2.md).
- Room loss during an active healthy non-tunneled GGPO fight degrades
  (fight continues; rematch/results/verification disabled; safe exit at match
  end) instead of closing GGPO. Legacy-tunnel matches still fail fully.
- Diagnostics (`SF4E_ROLLBACK_DIAGNOSTICS=1`) must never change simulation
  state and must stay allocation-free per frame.
- Never call `ggpo_close_session` from inside a GGPO callback. The fork deletes
  the backend and keeps calling the advance-frame callback afterwards. Every
  callback body is wrapped in `GgpoCallbackScope`; an abort raised inside one
  latches and `fSystem::DrainPendingAbort()` completes it after the top-level
  GGPO call returns (see `sf4e__GgpoAbortLatch.hxx`).
- No abort, failure or retirement writes `bUpdateAllowed`. Without a session
  only the manual gate applies (`GgpoGateModel::MayAdvance`), so an abort can
  never leave an offline battle, or an orphan that has to leave, frozen.
- A spectator handle's `DISCONNECTED_FROM_PEER`, and a spectator's v1 snapshot
  or v2 hash mismatch, never end the two fighters' game.
- Spectators never hold the fighters. The start barrier is the two fighters
  once P1 accepts the grant's `spectators_optional` offer; P1 waits at most
  `IrohMatchSession::SpectatorGraceMs` for spectator links and reports the ones
  that came up in `game_ready.slots`, and the authority sends the rest
  `game_end` for that generation. An older P1 keeps the all-participant
  barrier. A terminal receipt holds its table only until both fighters
  acknowledge; a spectator still acknowledging sits the next generation out.
  On P1, `SpectatorPolicy` drops a spectator GGPO has not synchronized within
  3 s, or one 32 or more frames unacknowledged on two consecutive samples.
- Room checkpoint decoding runs on `CheckpointDecodeWorker`, the only room
  worker thread. It takes bytes and returns values; it never touches
  `SessionServer`, `RoomAuthority`, `IrohRoom` or GGPO, and commits are still
  staged and imported in order on the game thread. `SF4E_ROOM_WORKER=0`
  decodes inline.
- A room snapshot to a client that joined with `roomChatDelta` may carry
  `chat_unchanged` instead of `chat`; the client keeps its chat. Chat versions
  a client holds are recorded when the candidate commits, never at journal
  time, so a superseding snapshot in the same candidate still carries chat.
- A chat line's sender is always a current member: `Leave` prunes the lines.
- A committed effect already in the activated journal is delivered whatever
  its term. The term fence applies only to effects not yet activated, which a
  stale leader may never commit.
- `fSystem::BattleUpdate` pumps `ggpo_idle(ggpo, 0)` before adding local
  input. Do not remove it: without it a remote input that arrived after the
  post-render poll is used one frame late.
- Every netplay alert goes through `NetplayFacade::PushAlert(text, severity)`
  and is shown by the match HUD state line; do not add a second channel.

## Relay mode

- Relay only changes how UDP reaches peers; it must not alter memento save/load or advance-frame callback semantics.
- When relay is off, behavior matches direct `memberData.ip` / `memberData.port` GGPO endpoints.
- UDP registration succeeds only on `SF4R`; `SF4W` means the peer has not registered the same match generation.
- V3 registration identifies the player by lobby slot and the match by both `readyMessageNum` values. Do not infer v3 slots from public IP or declared local port.
- A pending rematch generation must not replace the active forwarding pair until both player slots register it.
- Registration and GGPO use the same local UDP port; release the previous GGPO session before registration and preserve the VPS-observed NAT endpoint.
- Treat every datagram on the GGPO port as untrusted. Short, unknown-type, and structurally invalid packets must be dropped before GGPO logs or dispatches them; network input must never trigger an assertion.
- VPS UDP sync failures abort to the lobby; never switch one peer automatically to the legacy session tunnel.
