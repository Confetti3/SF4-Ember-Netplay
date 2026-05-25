# SF4 Enhanced v0.2.0

Relay-first rollback netplay for **Ultra Street Fighter IV** (Steam) with a modern **SF4 Enhanced** launcher.

## What's new

- **Simple mode** (default): relay room codes **`SF4-XXXX`** — no manual broker setup for testers
- **`RelayHost.exe`** runs on the **host's PC** when they start a game (broker on Oracle VPS)
- Preconfigured room broker: `http://150.136.121.155:8787` (override with `SF4E_BROKER_URL`)
- Advanced mode: direct IP, UPnP, custom broker URL
- Find match / Open rooms stubs (early)

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
| **Start game** (starts `RelayHost.exe`) | **Start game** |
| Forward **TCP+UDP 23456** (or UPnP in Advanced) | No port forward needed |
| Both **Ready** in-game | Same zip on both PCs |

## Broker override

Default broker is baked into the launcher. To use another broker:

```text
set SF4E_BROKER_URL=http://your-broker:8787
```

If you upgraded from an older build, delete or edit `%APPDATA%\sf4e\config.json` if the broker URL is still `127.0.0.1`.

## Known limitations

- Host must reach the internet and expose port **23456** (relay runs on host PC, not the VPS)
- Both players must use the **same release zip** (`Sidecar.dll` must match)
- Find match / Open rooms are early stubs

## Support

Include the **Git** line from `BUILD_INFO.txt` and a screenshot when reporting issues.
