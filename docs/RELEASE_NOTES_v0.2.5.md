# SF4 Enhanced v0.2.5

Relay-first rollback netplay for **Ultra Street Fighter IV** (Steam) with a modern **SF4 Enhanced** launcher.

## What's new

- **Join preflight:** launcher checks host reachability before launching USF4; clear error if host has not forwarded the assigned relay port
- **Join overlay:** join-specific connecting messages (host must Start game + forward TCP+UDP on assigned port)
- **Simple mode** (default): relay room codes **`SF4-XXXX`** — no manual broker setup for testers
- **`RelayHost.exe`** runs on the **host's PC** when they start a game
- Preconfigured room broker: `http://74.208.200.95:8787` (override with `SF4E_BROKER_URL`)
- Advanced mode: direct IP, UPnP, custom broker URL
- In-app **Check for updates** from GitHub Releases

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
| Both **Ready** in-game | Same zip on both PCs |

## Broker override

Default broker is baked into the launcher. To use another broker:

```text
set SF4E_BROKER_URL=http://your-broker:8787
```

If you upgraded from an older build, delete or edit `%APPDATA%\sf4e\config.json` if the broker URL is still the old Oracle VPS.

## Known limitations

- Host must reach the internet and expose the broker-assigned relay port (23456–23475; relay runs on host PC, not the VPS)
- Joiner launcher preflight uses TCP; host still needs **TCP+UDP** forwarded for gameplay
- Both players must use the **same release zip** (`Sidecar.dll` must match)
- Find match / Open rooms are early stubs

## Support

Include the **Git** line from `BUILD_INFO.txt` and a screenshot when reporting issues.
