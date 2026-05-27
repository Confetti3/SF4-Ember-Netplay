# SF4 Netplay Launcher v0.2.7.3

Bugfix release — fixes VPS relay black screen after character portraits.

## What's fixed in v0.2.7.3

- **VPS relay matches:** GGPO now tunnels through the session relay correctly instead of attempting blocked direct UDP between players. Fixes the black screen that appeared after portraits when both players were connected in lobby.
- **Root cause:** `GgpoRelay` virtual endpoints were configured after GGPO started, so relay mode silently fell back to peer public IP:port.

Includes all v0.2.7 / v0.2.7.1 / v0.2.7.2 features. **Direct IP (Advanced) launcher routing unchanged from v0.2.6.**

## Upgrade note

Update **both** `Launcher.exe` and `Sidecar.dll` together on **both PCs** (same zip). NetplayConfig stays at version 3.

After updating, a successful VPS match should show in `%APPDATA%\sf4e\logs\sf4e.log`:

```
GgpoRelay: started localGgpoPort=23457 peers=1
GgpoRelay: remote endpoint 127.0.0.1:40000
GGPO: Connected!
GGPO: Running
```
