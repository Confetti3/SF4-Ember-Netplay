# SF4 Enhanced v0.2.6

Rollback netplay for **Ultra Street Fighter IV** (Steam) with a modern **SF4 Enhanced** launcher.

## What's new in v0.2.6

- **Direct IP fix:** restores v0.2.2-style direct play — default connect mode is Direct IP again; joiners pasting `public.ip:23456` no longer hit the room broker; stale `SF4-XXXX` codes no longer force relay when the host selects Direct IP
- **In-app updater fix:** downloads to `%TEMP%` reliably, applies updates via `apply-update.ps1` after the launcher exits (fixes silent update failures on v0.2.5 builds)
- **WAN direct hints:** Advanced host/join screens explain public IP:port sharing and port-forward requirements

Also includes v0.2.5 changes: join preflight for relay codes, join overlay improvements, hardened in-app updater (curl/TLS fallbacks, Open release page on failure).

## Based on sf4e

This build is a fork of [sf4e](https://codeberg.org/adanducci/sf4e) by Anthony D'Anducci and contributors (MIT). See `ATTRIBUTION.md` in the zip.

## Prerequisites

- **Ultra Street Fighter IV** on Steam (app 45760) — not included
- [Microsoft Edge WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
- [VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe)

## Install

1. Download the **team zip** asset below (not "Source code" only).
2. Extract the **entire** zip to one folder.
3. Run `powershell -ExecutionPolicy Bypass -File preflight.ps1`
4. Run `Launcher.exe` — **Host**, **Join**, or **Offline**

## Quick start (relay)

| Host | Joiner |
|------|--------|
| Simple mode → **Create relay room** | Paste **`SF4-XXXX`** from host |
| **Start game** (starts `RelayHost.exe`) | Wait for host **Connected**, then **Start game** |
| Forward **TCP+UDP** on assigned port (23456–23475; share hint shows exact port) | No port forward needed |

## Quick start (direct IP over internet)

| Host | Joiner |
|------|--------|
| **Advanced** → **Direct IP** → enter **public IP** | **Advanced** → **Direct IP:port** → paste `host.public.ip:23456` |
| Forward **TCP+UDP** on session port (default 23456) | No port forward needed |
| Share the **Public address** card | Host must use Direct IP (not relay) |

If the launcher keeps defaulting to Relay, delete `%APPDATA%\sf4e\config.json` and reinstall this zip.

## Broker override

```text
set SF4E_BROKER_URL=http://your-broker:8787
```

## Known limitations

- Relay host must forward the broker-assigned port; direct host must forward the session port
- Both players must use the **same release zip** (`Sidecar.dll` must match)

## Support

Include the **Git** line from `BUILD_INFO.txt` and a screenshot when reporting issues.
