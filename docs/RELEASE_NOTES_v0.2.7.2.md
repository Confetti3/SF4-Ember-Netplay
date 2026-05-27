# SF4 Netplay Launcher v0.2.7.2

UX transparency pass — clearer VPS relay copy; Direct IP unchanged.

## What's new in v0.2.7.2

- **In-game overlay:** VPS host/join shows the actual **SF4-XXXX** room code (not "share public IP:port" or placeholder text)
- **NetplayConfig v3:** launcher passes `relayRoomCode` to Sidecar for overlay display
- **Simple mode host UI:** hides LAN/public-IP cards when VPS relay room is active (relay code only)
- **Broker errors:** clearer player-facing messages ("room broker" instead of "room service")
- **Docs:** README and USER_NETPLAY aligned with VPS-first Simple mode; Direct IP port-forward docs preserved for Advanced

Includes all v0.2.7 / v0.2.7.1 VPS relay features. **Direct IP (Advanced) behavior unchanged from v0.2.6.**

## Upgrade note

Update **both** `Launcher.exe` and `Sidecar.dll` together (NetplayConfig version 3).
