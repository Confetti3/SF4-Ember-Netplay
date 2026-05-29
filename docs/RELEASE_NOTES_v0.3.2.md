# SF4 Netplay Launcher v0.3.2

Fix UDP GGPO relay fallback + in-game transport display. Pairs with VPS broker update (connect-plan `roomToken`). Includes Defender false-positive guide (`docs/WINDOWS_DEFENDER.md`).

> **Not production-ready.** Experimental unofficial port for a small friends group.

## Prerequisites (install on each PC before playing)

**These are not included in the zip.** Install once per machine:

| Requirement | Why | Download |
|-------------|-----|----------|
| **Ultra Street Fighter IV** (Steam app **45760**) | The game this launcher hooks into | [Steam](https://store.steampowered.com/app/45760/) |
| **Microsoft Edge WebView2 Runtime** | Launcher UI (Host / Join / Offline) | [WebView2](https://go.microsoft.com/fwlink/p/?LinkId=2124703) |
| **Microsoft Visual C++ Redistributable (x86)** | `Launcher.exe`, `Sidecar.dll`, and relay binaries | [VC++ x86](https://aka.ms/vs/17/release/vc_redist.x86.exe) |

**OS:** Windows 10 or later (64-bit Windows; the launcher is **32-bit/x86** to match USF4).

**After you extract the zip:**

1. Run **`preflight.cmd`** in the folder (checks WebView2, VC++, and that all files are present).
2. Run **`Launcher.exe`**.

**Netplay:** Both players must use this **exact zip** (`BUILD_INFO.txt` Git line must match). Do not mix with v0.3.1 or other builds.

**Broker (Simple mode):** `https://74-208-200-95.nip.io` (launcher Advanced if you need to set it manually).

## What's new

### VPS (deploy with this release)
- **Connect-plan includes `roomToken`** — clients can register on the GGPO UDP relay without a separate register-endpoint round-trip for the token
- Restart `sf4e-broker` after uploading `server.js` (see `scripts/update-dashboard-vps.py`)

### Launcher / Sidecar (this zip)
- **NAT probe no longer wipes connect plan** — failed probe keeps `udp_relay` from broker instead of forcing legacy tunnel
- **Host/guest fallbacks** — session `roomToken` + GGPO port applied when connect-plan fetch fails
- **UDP relay DNS resolve** — registration works with broker hostname, not IP-only
- **In-game Network panel** — shows effective GGPO path: **UDP relay** (green) vs **Legacy session tunnel** (yellow)

## Install

1. Download **`sf4-netplay-launcher-*-v0.3.2.zip`** below (not "Source code").
2. Extract the **entire** zip to one folder (keep all DLLs and `launcher-ui/` next to `Launcher.exe`).
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.

## Quick start (Simple mode)

| Host | Joiner |
|------|--------|
| **Host** -> **Get code** | **Join** -> paste **`SF4-XXXX`** |
| **Start game** | Wait, then **Start game** |
| **Ready** in lobby | **Ready** |

During a match, open **Network** — you should see **GGPO path: UDP relay** (not legacy tunnel) when auto transport succeeds.

## Windows Defender (`Wacapew.A!ml`)

**Pre-release — not recommended for general install.** Use [v0.3.1](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/tag/v0.3.1) until a signed build ships.

Some PCs flag **`Sidecar.dll`** as `Program:Win32/Wacapew.A!ml` (heuristic false positive on unsigned Detours hook). See `docs/WINDOWS_DEFENDER.md` in the zip.


## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) � black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, and logs.

## Bug reports

Include `BUILD_INFO.txt` Git line, `%APPDATA%\sf4e\logs\sf4e.log`, room code, broker URL, and the **GGPO path** line from the Network panel.
