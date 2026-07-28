# Ultra Street Fighter IV — SF4 Netplay Launcher (player guide)

**Something broken?** See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for black launcher screen, crash on **Start game**, recommended settings, and Direct IP firewall help.

> **Experimental unofficial port** — based on upstream [sf4e](https://codeberg.org/adanducci/sf4e) by Anthony Danducci (MIT). Anthony Danducci does not maintain or endorse this launcher. **Not production-ready** — netplay may fail. See `ATTRIBUTION.md`.

## Prerequisites

- **[VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe)** — required for the launcher and Sidecar binaries.
- **Qt 6 runtime** — included in the release zip (`Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `plugins/`). No separate UI runtime install needed.

## Quick start

1. Extract a release build so `Launcher.exe`, `Sidecar.dll`, `RelayHost.exe`, Qt DLLs, and `plugins/` sit in the same directory.
2. Run **Launcher.exe**.
3. In the Qt launcher home screen, pick **Host**, **Join**, or **Offline** (toggle **Advanced** for direct IP, UPnP, open rooms, and broker URL).
4. Click **Start game** — USF4 launches with netplay configured.

## Simple mode (relay — experimental, friends-only)

**Simple mode** is on by default. Use short room codes — no manual IP entry.

### Host (relay)

1. **Host** (home → Host) → enter display name.
2. Click **Get code** → copy **`SF4-XXXX`** and send it to your opponent.
3. Click **Start game** — you connect to the VPS (no port forward on your PC). The in-game overlay shows your **SF4-XXXX** code to share.
4. Wait in the in-game lobby; both players **Ready** to start.

### Join (relay)

1. **Join** (home → Join) → enter display name.
2. Paste the host's **`SF4-XXXX`** code.
3. Wait until the host has clicked **Start game**, then click **Start game** on your side.
4. Press **Ready** in the lobby when connected.

Default broker: `https://74-208-200-95.nip.io`. Override with `SF4E_BROKER_URL` or Advanced → **Room broker URL**. For a local HTTP broker during development, also set `SF4E_ALLOW_HTTP_BROKER=1`.

### Transport (Simple mode)

After **Start game**, the launcher fetches a connect-plan, probes NAT (`8790/udp`), and registers endpoints. Sidecar then picks the best path:

```
p2p → udp_relay → legacy_session_tunnel (always available fallback)
```

Optional override: `SF4E_GGPO_TRANSPORT=legacy|udp|p2p|auto`. Details: [TRANSPORT_REGRESSION.md](TRANSPORT_REGRESSION.md).

## Host (Advanced — direct IP)

1. Select **Host**, enter display name and input delay.
2. The launcher shows your **LAN address** (same Wi‑Fi/Ethernet) and your **public internet address** (use **Refresh** if needed; you can edit it).
3. Click **Copy** on the room code and send it to your opponent (format `IP:port`, default port **23456**).
4. **Internet play:** joiners must use your **public** or **VPN** IP in the room code, not `192.168.x.x`, unless they are on the same LAN.
5. On your router, **port-forward TCP and UDP** on the session port to this PC. Allow the port in **Windows Firewall**.
6. Wait in the in-game lobby; when both players are **Ready** on the main menu, the match starts.

## Join (Advanced — direct IP)

1. Select **Join**, enter your display name.
2. Paste the host's room code into **Host address** (`IP:port` or `hostname:port`). The field remembers your last join.
3. You can use a **different IP** than the host's LAN address (e.g. their public IP or a VPN IP).
4. Click **Start game**; the Network panel shows your **join target**. Press **Ready** in the lobby when connected.

## Offline

Launches USF4 with the Sidecar netplay layer loaded but no online session.

## Command line (optional)

```bat
Launcher.exe --host
Launcher.exe --join SF4-AB12
Launcher.exe --join 203.0.113.42:23456
Launcher.exe --offline
Launcher.exe --console
Launcher.exe --dev-overlay
Launcher.exe --apply-update
```

Automation / legacy bridges (not for normal play): `--electron-ipc`, `--headless-test-handshake`.

## Playing over the internet (WAN)

| Requirement | Notes |
|-------------|--------|
| Same `Sidecar.dll` on both PCs | Join fails with "version mismatch" otherwise |
| Host port-forward **23456** | **Simple relay:** no host port forward. **Direct IP (Advanced):** host forwards session port (default 23456, TCP+UDP) |
| Qt 6 DLLs in zip | Included in the release package |
| Room code **`SF4-XXXX`** | Default in Simple mode; broker resolves to VPS relays |
| Direct **`IP:port`** | Advanced mode only |

**Simple mode:** prefer **p2p** or **udp_relay**; **legacy_session_tunnel** always works as fallback. Set `SF4E_RELAY=0` only for advanced LAN troubleshooting.

## Build ID (version check)

Both players need the same `Sidecar.dll` build. Enable developer overlay (`SF4E_NETPLAY_DEV=1` or `--dev-overlay`) to see the build hash in-game.

## Graphics

Disable **Smooth** frame rate in in-game graphics options if rollback feels wrong.

## Logs

`Launcher.exe --console` or files under `%APPDATA%\sf4e\`.

Full checklist and bug-report items: [TROUBLESHOOTING.md](TROUBLESHOOTING.md#collecting-logs-for-a-bug-report).

## Linux / Steam Deck

Use `protontricks-launch --appid 45760 Launcher.exe` as described in the main README.
