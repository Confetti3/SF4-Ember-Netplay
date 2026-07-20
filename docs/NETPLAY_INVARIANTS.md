# sf4e netplay invariants

Do not change these behaviors without regression testing (SessionInteractiveTest, LAN 2P, relay 2P).

## GGPO (`sf4e__Game__Battle__System.cxx`)

- `ggpo_start_session` / `ggpo_start_spectating` use the same callback set: `begin_game`, `advance_frame`, `save_game_state`, `load_game_state`, `free_buffer`, `on_event`, `log_game_state`.
- `ggpo_advance_frame_callback` calls **undetoured** `rSystem::BattleUpdate`, never `fSystem::BattleUpdate`.
- Savestates use the preallocated `saveStates[]` pool; `ggpo_save_game_state_callback` sets `*len = 1`; `ggpo_free_buffer` releases slots.
- Local input: `ggpo_add_local_input` in `fSystem::BattleUpdate`; rollback uses `ggpo_synchronize_input` and `fPadSystem::playbackData`.
- When `bUsePureSounds` is enabled, call `fSoundPlayerManager::SyncState()` around simulation steps.
- Create the GGPO session in `fUserApp::_OnVsBattleTasksRegistered` (after VsBattle load), not earlier.
- Default disconnect timeouts: **3000 ms / notify 1500 ms** (`NetplayConfig` v8+). Older launchers derive timeout from host input delay (`max(3000, 1000 + delay*500)`).

## Session protocol (`sf4e__SessionProtocol.hxx`)

- Preserve JSON message `type` strings and field names for lobby, prebattle, battle sync, snapshots, and punch signaling (`punch_ready`, `punch_go`).
- Keep `sidecarHash` join validation (`JR_HASH_INVALID`).
- Desync detection via `StateSnapshot` exchange must remain enabled in normal play.

## Rollback remediation (2026-07, feat/rollback-diagnostics-and-pacing)

- `bUpdateAllowed` carries only lifecycle gating, manual/debug pause, terminal
  failure, and room failure — read via `fSystem::MayAdvanceDeterministicFrame()`.
  Connection warnings must NOT freeze simulation; prediction stalls are enforced
  by GGPO refusing local input (`GgpoGateModel`, `ClassifyGgpoResult`).
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
  `battle_snapshot` system stays active (see docs/DESYNC_V2.md).
- Room loss during an active healthy non-tunneled GGPO fight degrades
  (fight continues; rematch/results/verification disabled; safe exit at match
  end) instead of closing GGPO. Legacy-tunnel matches still fail fully.
- Diagnostics (`SF4E_ROLLBACK_DIAGNOSTICS=1`) must never change simulation
  state and must stay allocation-free per frame.

## Relay mode

- Relay only changes how UDP reaches peers; it must not alter memento save/load or advance-frame callback semantics.
- When relay is off, behavior matches direct `memberData.ip` / `memberData.port` GGPO endpoints.
- UDP registration succeeds only on `SF4R`; `SF4W` means the peer has not registered the same match generation.
- V3 registration identifies the player by lobby slot and the match by both `readyMessageNum` values. Do not infer v3 slots from public IP or declared local port.
- A pending rematch generation must not replace the active forwarding pair until both player slots register it.
- Registration and GGPO use the same local UDP port; release the previous GGPO session before registration and preserve the VPS-observed NAT endpoint.
- Treat every datagram on the GGPO port as untrusted. Short, unknown-type, and structurally invalid packets must be dropped before GGPO logs or dispatches them; network input must never trigger an assertion.
- VPS UDP sync failures abort to the lobby; never switch one peer automatically to the legacy session tunnel.
