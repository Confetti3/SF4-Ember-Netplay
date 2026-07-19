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

## Relay mode

- Relay only changes how UDP reaches peers; it must not alter memento save/load or advance-frame callback semantics.
- When relay is off, behavior matches direct `memberData.ip` / `memberData.port` GGPO endpoints.
- UDP registration succeeds only on `SF4R`; `SF4W` means the peer has not registered the same match generation.
- V3 registration identifies the player by lobby slot and the match by both `readyMessageNum` values. Do not infer v3 slots from public IP or declared local port.
- A pending rematch generation must not replace the active forwarding pair until both player slots register it.
- Registration and GGPO use the same local UDP port; release the previous GGPO session before registration and preserve the VPS-observed NAT endpoint.
- VPS UDP sync failures abort to the lobby; never switch one peer automatically to the legacy session tunnel.
