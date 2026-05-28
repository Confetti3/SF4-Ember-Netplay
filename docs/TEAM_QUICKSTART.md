# SF4 Netplay Launcher — team test package

> **Experimental unofficial port** — this is **not** the official [sf4e](https://codeberg.org/adanducci/sf4e) project by Anthony Danducci. **Not production-ready.** See `ATTRIBUTION.md`.

This folder is a **self-contained experimental port** for netplay testing with friends. It does not include the game itself. **Expect bugs and failed sessions.**

**Official upstream:** [sf4e on Codeberg](https://codeberg.org/adanducci/sf4e) — use that for Anthony Danducci's official project and updates.

## Windows Defender says “Wacapew” or blocks install

This is a **known false positive** on unsigned netplay tools that inject into USF4 (`Sidecar.dll`). It is **not** confirmed malware.

1. Download only from [GitHub Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest).
2. In the alert, use **Allow on device** (or restore quarantined files).
3. Optional: add your **install folder** as a Defender exclusion.

Full explanation: [`docs/WINDOWS_DEFENDER.md`](WINDOWS_DEFENDER.md) in this package.

## Scope and limitations

| In scope | Limits |
|----------|--------|
| USF4 on Steam, Windows 10+ | Game **not included** |
| Simple VPS room codes (`SF4-XXXX`) | **Experimental** — not official sf4e |
| Same zip on all players | Shared broker (~**20 rooms**, ~**15 min** idle expiry) |
| Advanced Direct IP / UPnP | Host port-forward for Direct IP; Find match / Open rooms **experimental** |

Full list: [`docs/SCOPE_AND_LIMITATIONS.md`](SCOPE_AND_LIMITATIONS.md) in this folder.

## Quick start (3 steps)

1. **Extract** the entire zip to one folder (e.g. `C:\Games\SF4-Netplay-Launcher\`). Do not copy only `Launcher.exe`.
2. **Install once** (if you have not already):
   - [Microsoft Edge WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
   - [VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe)
   - **Ultra Street Fighter IV** on Steam (app ID 45760)
3. **Run** `preflight.ps1` (optional sanity check), then **`Launcher.exe`** from that folder → **Host**, **Join**, or **Offline**.

## Experimental testers

Inviting friends? Share [`docs/BETA_TESTERS.md`](BETA_TESTERS.md) — same zip on both PCs, current `SF4-XXXX` from host, log paths for bug reports.

## Updating

On the launcher home screen, use **Check for updates** to compare your install against the latest [GitHub release](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest). If a newer build is available, **Install update** downloads the team zip and replaces all files in the install folder (your settings in `%APPDATA%\sf4e\` are kept). Close USF4 before installing.

```powershell
preflight.cmd
```

## Casual netplay (Simple mode — default)

The launcher defaults to **Simple mode** and relay room codes (`SF4-XXXX`). Broker **`http://74.208.200.95:8787`** is preconfigured (VPS session relay — no host port forward).

| Host | Joiner |
|------|--------|
| **Create relay room** → copy `SF4-XXXX` | Paste code → **Start game** |
| **Start game** (connects outbound to VPS relay) | Wait for host **Connected**, then join |
| No port forward on host PC | No port forward needed |

See [CASUAL_NETPLAY.md](CASUAL_NETPLAY.md) in `docs/`. Override broker: `set SF4E_BROKER_URL=http://your-broker:8787`.

## Direct IP over internet (Advanced mode)

Use when you prefer port-forward on the host instead of relay room codes (same as v0.2.2 direct play).

| Host | Joiner |
|------|--------|
| **Advanced** → **Direct IP** → enter **public IP** in Internet address | **Advanced** → **Direct IP:port** → paste `host.public.ip:23456` |
| Forward **TCP+UDP** on **session port** (default 23456) | No port forward needed |
| Share the **Public address** card on the host screen | Host must use Direct IP (not relay) |

If join fails, confirm both players use **Advanced** mode and the joiner pasted **IP:port**, not `SF4-XXXX`.

## What you need for netplay

- **Relay (recommended for WAN):** broker + session relay on VPS **`74.208.200.95`** — host and joiner connect outbound; no host port forward.
- **Direct (Advanced):** same LAN or host port-forwards session port **23456** (TCP/UDP).
- **Same zip on both players** — do not mix `Sidecar.dll` from another build.

## File checklist

Confirm these sit **next to each other** in one folder:

- `Launcher.exe`, `Sidecar.dll`, **`RelayHost.exe`**, `WebView2Loader.dll`
- `launcher-ui\` (`index.html`, `app.js`, `styles.css`)
- `spdlog.dll`, `fmt.dll`, `GameNetworkingSockets.dll`, `GGPO.dll`, `libcrypto-3.dll`, `libprotobuf.dll`, `abseil_dll.dll`

See `MANIFEST.txt` in the package to verify extraction.

## Run the launcher

1. Double-click **`Launcher.exe`** (or `Launcher.exe --console` for logs).
2. In the WebView2 launcher (Simple mode):
   - **Host** — **Create relay room** → **Copy code** → **Start game**
   - **Join** — paste **`SF4-XXXX`** → **Start game**
   - **Offline** — game only, no netplay session
3. Click **Start game**. sf4e should find USF4 via Steam automatically.

If Steam detection fails:

```bat
set STEAM_APP_PATH=C:\Program Files (x86)\Steam\steamapps\common\Super Street Fighter IV - Arcade Edition
Launcher.exe
```

## Firewall

- **Relay host (Simple mode):** no router or Windows firewall setup on the host PC — traffic goes through the VPS.
- **VPS operator:** open **8787/tcp** plus **23456–23475/tcp+udp** in both **ufw** and the **IONOS (or provider) cloud firewall**. Missing IONOS UDP rules causes in-game “Still connecting…” even when `relay-diag.ps1` passes.
- **Direct IP host:** forward **TCP+UDP** on session port (default 23456).
- **Joiner:** no port forward needed for relay mode.

## Quick 2-player test

| Step | Host | Joiner |
|------|------|--------|
| 1 | **Create relay room** → **Start game** | Paste **SF4-XXXX** → **Start game** |
| 2 | Wait in lobby | Connect |
| 3 | Both **Ready** in-game | Both **Ready** |
| 4 | Play a few rounds | Same zip / `Sidecar.dll` |

Full checklist: [SMOKE_TEST.md](SMOKE_TEST.md). Player guide: [USER_NETPLAY.md](USER_NETPLAY.md).

## Troubleshooting

| Problem | What to try |
|---------|-------------|
| `WebView2Loader.dll was not found` | Re-extract the **full** zip; keep all files beside `Launcher.exe` |
| Launcher says WebView2 required | Install [WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703) |
| Blank launcher window | Ensure **`launcher-ui\`** is next to `Launcher.exe` |
| Double-click does nothing | Run `Launcher.exe --console` from a terminal in the package folder |
| “Version mismatch” on join | Same zip on both PCs |
| Can't create relay room | Broker reachable? `curl http://74.208.200.95:8787/v1/health` |
| Joiner stuck / "Cannot reach relay" | Host must **Start game** first. Check broker: `curl http://74.208.200.95:8787/v1/health` (should show `"forceVpsRelay":true`). |
| In-game "Still connecting" (VPS relay) | Open **IONOS inbound UDP 23456–23475** (not just ufw). Run `scripts\relay-diag.ps1`; create a **new** room after firewall fix. |
| Direct IP join fails | Both use **Advanced** → **Direct IP**. Joiner pastes `public.ip:port` (not SF4-XXXX). Host forwards **session port** TCP+UDP. Delete `%APPDATA%\\sf4e\\config.json` if mode keeps resetting to Relay. |
| In-app update "Download failed" | Use **Open release page** in launcher, or download zip manually once. Log: `%TEMP%\sf4e-update.log`. Test: `powershell -File scripts\test-updater-download.ps1` |
| Join times out in-game | Same build on both PCs; host clicked **Start game**; broker health OK |
| Room expires while waiting | Deploy updated `server.js` on Oracle (adds `/heartbeat`); launcher sends keepalive every 60s |
| Wrong broker URL | Delete `%APPDATA%\sf4e\config.json` or set `SF4E_BROKER_URL` |
| Missing other DLL errors | Re-extract full zip; install [VC++ x86](https://aka.ms/vs/17/release/vc_redist.x86.exe) |
| Host/Join issues at menu | Open in-game **Network** panel; both **Ready** |
| Need logs | `Launcher.exe --console` or `%APPDATA%\sf4e\` |

Report issues with the **Git** line from `BUILD_INFO.txt` and a screenshot.

## Developer overlay (optional)

```bat
set SF4E_NETPLAY_DEV=1
Launcher.exe --dev-overlay
```

## Linux / Steam Deck

Use Proton: `protontricks-launch --appid 45760 Launcher.exe` from this folder.
