# SF4 Netplay Launcher v0.2.7.3

VPS relay rollback netplay for **Ultra Street Fighter IV** (Steam) with a modern **SF4 Netplay Launcher** UI.

## What's new

- **Simple mode** (default): VPS relay room codes **`SF4-XXXX`** — no port forward on the host PC
- Session relay runs on the broker VPS; host and joiner connect outbound
- **Advanced mode:** Direct IP, UPnP, custom broker URL (unchanged from v0.2.6 routing)
- In-app updater: **Check for updates** on the launcher home screen

## Unofficial port (not official sf4e)

This is an **unofficial port** of [sf4e](https://codeberg.org/adanducci/sf4e) by **Anthony Danducci** and contributors (MIT). It is **not** the upstream project and is **not** endorsed by Anthony Danducci. See `ATTRIBUTION.md` in the zip. For official sf4e, use [Codeberg](https://codeberg.org/adanducci/sf4e).

## Prerequisites

- **Ultra Street Fighter IV** on Steam (app 45760) — not included
- [Microsoft Edge WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
- [VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe)

## Install

1. Download the **team zip** asset below (not "Source code" only).
2. Extract the **entire** zip to one folder.
3. Run `preflight.cmd`
4. Run `Launcher.exe` — **Host**, **Join**, or **Offline**

## Quick start (Simple VPS relay)

| Host | Joiner |
|------|--------|
| **Create relay room** → copy **`SF4-XXXX`** | Paste code from host |
| **Start game** | Wait for host **Connected**, then **Start game** |
| No port forward on host PC | No port forward needed |
| Both **Ready** in-game | **Same release zip** on both PCs |

See `docs/BETA_TESTERS.md` in the zip for the full beta checklist.

## Broker override

Default broker is baked into the launcher (`http://74.208.200.95:8787`). To use another broker:

```text
set SF4E_BROKER_URL=http://your-broker:8787
```

## Known limitations

See [docs/SCOPE_AND_LIMITATIONS.md](docs/SCOPE_AND_LIMITATIONS.md) for the full list. Summary:

- Both players must use the **same release zip** (`Sidecar.dll` must match)
- **Find match** and **Open rooms** are experimental — use **Host + room code** for beta testing
- VPS relay supports up to **20** concurrent rooms on the default broker; idle rooms expire after ~15 minutes
- Advanced Direct IP still requires host **TCP+UDP** port forward on the session port
- Rematch, disconnect recovery, and spectator mode need more beta coverage

## Support

Include the **Git** line from `BUILD_INFO.txt`, `%APPDATA%\sf4e\logs\sf4e.log`, and a screenshot when reporting issues.
