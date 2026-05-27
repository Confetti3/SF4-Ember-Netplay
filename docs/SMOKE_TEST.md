# sf4e netplay smoke test checklist

Run after netplay or UX changes.

## Automated

- [ ] Build `SessionInteractiveTest` and run: two clients connect, both ready, `OnReady` fires.
- [ ] CMake build succeeds for `Launcher` and `Sidecar` (x86).

## Manual — session

- [ ] Host from launcher: room code displayed, game reaches main menu, session connects.
- [ ] Join from second machine or VM with room code; join succeeds, names appear in lobby.
- [ ] Version mismatch: different `Sidecar.dll` → clear "version mismatch" message.

## Manual — match

- [ ] LAN 2P: three rounds, no desync dialog, rollback audio acceptable.
- [ ] Relay 2P (default): NAT or without GGPO port forward still connects.
- [ ] Transport modes: see [TRANSPORT_REGRESSION.md](TRANSPORT_REGRESSION.md) when changing GGPO transport.
- [ ] Rematch: second game starts without snapshot/desync spam.
- [ ] Disconnect mid-match: returns to menu without crash.
- [ ] Spectator (3rd client in queue): can watch; host session stays up until spectator leaves or timeout.

## Manual — offline

- [ ] Launcher **Offline**: game runs, no auto netplay connection.
