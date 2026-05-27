# SF4 Netplay Launcher v0.3.0

Transport + TLS release — HTTPS broker, VPS port hardening, and GGPO transport groundwork.

> **Not production-ready.** Experimental unofficial port for a small friends group. **Prefer [v0.3.1](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest)** for current VPS auto transport.

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

**Netplay:** Both players must use the **same release zip** (`BUILD_INFO.txt` Git line must match).

**Broker (Simple mode):** `https://74-208-200-95.nip.io` (launcher Advanced if you need to set it manually).

## What's new

### VPS / broker
- **TLS via Caddy** — broker at `https://74-208-200-95.nip.io` (no plain `:8787` from the internet)
- **Port lockdown** — broker/dashboard/relay-manager on localhost only; public ports are 443 + game relay ranges
- GGPO UDP relay binary on VPS (for future auto transport)
- Match coordinator APIs + dashboard panel (operator use)

### Launcher
- Default broker URL: **`https://74-208-200-95.nip.io`**
- **HTTPS required** for public broker URLs (`SF4E_ALLOW_HTTP_BROKER=1` for dev only)
- NetplayConfig **v5** — GGPO transport fields, connect-plan, legacy fallback

## Install

1. Download **`sf4-netplay-launcher-*-0.3.0.zip`** below (not "Source code").
2. Extract the **entire** zip to one folder.
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.

## Operator

- Dashboard: `https://74-208-200-95.nip.io/login`
- See [VPS_TLS_SETUP.md](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/VPS_TLS_SETUP.md)

## Bug reports

Include `BUILD_INFO.txt` Git line, `%APPDATA%\sf4e\logs\sf4e.log`, and broker URL from Advanced settings.
