# SF4 Netplay Launcher v0.3.5 (testing)

**Pre-release for testing** — supersedes v0.3.4. **v0.3.1** remains the default stable download until Authenticode signing ships.

> **Experimental unofficial port** — friends-only testing. **Unsigned** build (Defender may flag `Sidecar.dll`).

## Install

1. Download **`sf4-netplay-launcher-*-v0.3.5.zip`** (Assets — not Source code).
2. Extract the **entire** zip on **both** PCs.
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.
4. Confirm both players show **v0.3.5** in the launcher header.

## What's fixed in v0.3.5

Includes all v0.3.4 UDP GGPO work, plus:

- **UDP registration:** accepts relay responses `SF4W` (waiting) and `SF4R` (ready).
- **Relay health:** `SF4H` probe before battle; both sides fall back to legacy tunnel if VPS GGPO relay is down.
- **VPS broker:** restarts GGPO UDP relay on heartbeat and connect-plan.
- **Sync safety:** no legacy-tunnel GGPO restart while already connected/synchronizing.
- **Overlay:** GGPO sync phase before **In match**.

## Verify (`%APPDATA%\sf4e\logs\sf4e.log`)

```
GgpoTransport: UDP relay registration OK ... localGgpoPort=23457
GGPO: Connected!
GGPO: Running
```

## Security

- No Defender exclusion scripts. Download only from this GitHub release page.


## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) � black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, and logs.

## Bug reports

Include `BUILD_INFO.txt`, room code, Network panel GGPO path, and `sf4e.log` from both PCs.
