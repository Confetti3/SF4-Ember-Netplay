# SF4 Netplay Launcher v0.3.1

Auto GGPO transport + NAT/connect-plan fixes. Pairs with VPS `BROKER_GGPO_TRANSPORT=auto`.

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

**Netplay:** Both players must use this **exact zip** (`BUILD_INFO.txt` Git line must match). Do not mix with v0.3.0 or other builds.

**Broker (Simple mode):** `https://74-208-200-95.nip.io` (launcher Advanced if you need to set it manually).

## What's new

### VPS (already deployed)
- **`BROKER_GGPO_TRANSPORT=auto`** — rooms spawn session + GGPO UDP relay pairs
- NAT probe **8790/udp** fixed (correct `SF4E_PROBE` handling)
- GGPO relay health probe fixed (`SF4OK` response)

### Launcher / Sidecar (this zip)
- **DNS resolution for NAT probe** — works with `https://74-208-200-95.nip.io` (not IP-only)
- Connect-plan + register-endpoint for **udp_relay** / **p2p** when broker is in auto mode
- Legacy session tunnel still used as fallback if UDP/P2P fails

## Install

1. Download **`sf4-netplay-launcher-*-v0.3.1.zip`** below (not "Source code").
2. Extract the **entire** zip to one folder (keep all DLLs and `launcher-ui/` next to `Launcher.exe`).
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.

## Quick start (Simple mode)

| Host | Joiner |
|------|--------|
| **Host** -> **Get code** | **Join** -> paste **`SF4-XXXX`** |
| **Start game** | Wait, then **Start game** |
| **Ready** in lobby | **Ready** |


## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) � black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, and logs.

## Bug reports

Include `BUILD_INFO.txt` Git line, `%APPDATA%\sf4e\logs\sf4e.log`, room code, and broker URL.
