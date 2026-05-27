# SF4 Netplay Launcher v0.2.7.3

VPS relay rollback netplay for **Ultra Street Fighter IV** (Steam) with a modern **SF4 Netplay Launcher** UI.

> **Experimental unofficial port** — not production-ready software. Friends-only testing; expect bugs and failed sessions.

## What's new

- **Simple mode** (default): VPS relay room codes **`SF4-XXXX`** — no port forward on the host PC
- Session relay runs on the broker VPS; host and joiner connect outbound
- **Advanced mode:** Direct IP, UPnP, custom broker URL (unchanged from v0.2.6 routing)
- In-app updater: **Check for updates** on the launcher home screen

## Unofficial port (not official sf4e)

This is an **unofficial port** of [sf4e](https://codeberg.org/adanducci/sf4e) by **Anthony Danducci** and contributors (MIT). It is **not** the upstream project and is **not** endorsed by Anthony Danducci. See `ATTRIBUTION.md` in the zip. For official sf4e, use [Codeberg](https://codeberg.org/adanducci/sf4e).

## Prerequisites (install on each PC before playing)

**These are not included in the zip.** Install once per machine:

| Requirement | Why | Download |
|-------------|-----|----------|
| **Ultra Street Fighter IV** (Steam app **45760**) | The game this launcher hooks into | [Steam](https://store.steampowered.com/app/45760/) |
| **Microsoft Edge WebView2 Runtime** | Launcher UI (Host / Join / Offline) | [WebView2](https://go.microsoft.com/fwlink/p/?LinkId=2124703) |
| **Microsoft Visual C++ Redistributable (x86)** | `Launcher.exe`, `Sidecar.dll`, and relay binaries | [VC++ x86](https://aka.ms/vs/17/release/vc_redist.x86.exe) |

**OS:** Windows 10 or later (64-bit Windows; the launcher is **32-bit/x86** to match USF4).

**After you extract the zip:** run **`preflight.cmd`**, then **`Launcher.exe`**.

**Netplay:** Both players must use the **same release zip** (`BUILD_INFO.txt` Git line must match).

(See also [docs/RELEASE_PREREQUISITES.md](RELEASE_PREREQUISITES.md) in the repo.)

## Install

1. Download the **team zip** asset below (not "Source code" only).
2. Extract the **entire** zip to one folder (keep all DLLs and `launcher-ui/` next to `Launcher.exe`).
3. Run **`preflight.cmd`**
4. Run **`Launcher.exe`** — **Host**, **Join**, or **Offline**

## Quick start (Simple VPS relay)

| Host | Joiner |
|------|--------|
| **Create relay room** → copy **`SF4-XXXX`** | Paste code from host |
| **Start game** | Wait for host **Connected**, then **Start game** |
| No port forward on host PC | No port forward needed |
| Both **Ready** in-game | **Same release zip** on both PCs |

See `docs/BETA_TESTERS.md` in the zip for the full experimental tester checklist.

## Broker override

Default broker is baked into the launcher (`http://74.208.200.95:8787`). To use another broker:

```text
set SF4E_BROKER_URL=http://your-broker:8787
```

## Known limitations

See [docs/SCOPE_AND_LIMITATIONS.md](docs/SCOPE_AND_LIMITATIONS.md) for the full list. Summary:

- Both players must use the **same release zip** (`Sidecar.dll` must match)
- **Find match** and **Open rooms** are experimental — use **Host + room code** for friends testing
- VPS relay supports up to **20** concurrent rooms on the default broker; idle rooms expire after ~15 minutes
- Advanced Direct IP still requires host **TCP+UDP** port forward on the session port
- Rematch, disconnect recovery, and spectator mode need more experimental testing coverage

## Support

Include the **Git** line from `BUILD_INFO.txt`, `%APPDATA%\sf4e\logs\sf4e.log`, and a screenshot when reporting issues.
