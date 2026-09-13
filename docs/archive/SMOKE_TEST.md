# sf4e netplay smoke test checklist

Run after netplay or UX changes.

## Automated (CTest)

From a configured CMake build directory:

```powershell
ctest -C RelWithDebInfo -R "GgpoUdpValidation|RollbackDiagnostics|GgpoGate|PacingController|StateHash|SaveStateOwnership" --output-on-failure
```

- [ ] `GgpoUdpValidation` — malformed / late GGPO UDP packets dropped safely
- [ ] `RollbackDiagnostics` — rollback diagnostics helpers
- [ ] `GgpoGate` — GGPO gate / session gate logic
- [ ] `PacingController` — pacing controller
- [ ] `StateHash` — state hash helpers
- [ ] `SaveStateOwnership` — savestate ownership / free-path safety
- [ ] Build `SessionInteractiveTest` and run manually: two clients connect, both ready, `OnReady` fires
- [ ] CMake build succeeds for `Launcher` and `Sidecar` (x86)

## Manual — session

- [ ] Host from launcher: **Get code**, room code displayed, game reaches main menu, session connects.
- [ ] Join from second machine or VM with room code; join succeeds, names appear in lobby.
- [ ] Version mismatch: different `Sidecar.dll` → clear "version mismatch" message.

## Manual — match

- [ ] LAN 2P: three rounds, no desync dialog, rollback audio acceptable.
- [ ] Relay 2P (default Simple mode): NAT or without GGPO port forward still connects.
- [ ] Transport modes: see [TRANSPORT_REGRESSION.md](TRANSPORT_REGRESSION.md) when changing GGPO transport.
- [ ] Rematch: second game starts without snapshot/desync spam.
- [ ] Disconnect mid-match: returns to menu without crash.
- [ ] Spectator (3rd client in queue): can watch; host session stays up until spectator leaves or timeout.

## Manual — offline

- [ ] Launcher **Offline**: game runs, no auto netplay connection.
