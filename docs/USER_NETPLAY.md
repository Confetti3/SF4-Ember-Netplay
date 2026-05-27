# Ultra Street Fighter IV — SF4 Netplay Launcher (player guide)

> **Unofficial port** — based on upstream [sf4e](https://codeberg.org/adanducci/sf4e) by Anthony Danducci (MIT). Anthony Danducci does not maintain or endorse this launcher. See `ATTRIBUTION.md`.

## Prerequisites

- **[Microsoft Edge WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703)** — required for the launcher UI (one-time install on Windows).
- **[VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe)** — required for the launcher and Sidecar binaries.

## Quick start

1. Extract a release build so `Launcher.exe`, `Sidecar.dll`, and the **`launcher-ui/`** folder sit in the same directory.
2. Run **Launcher.exe**.
3. In the WebView2 launcher, choose **Host**, **Join**, or **Offline**.
4. Click **Start game** — USF4 launches with netplay configured.

## Simple mode (relay — recommended)

**Simple mode** is on by default. Use short room codes — no manual IP entry.

### Host (relay)

1. **Host** → enter display name.
2. Click **Create relay room** → copy **`SF4-XXXX`** and send it to your opponent.
3. Click **Start game** — you connect to the VPS relay (no port forward on your PC). The in-game overlay shows your **SF4-XXXX** code to share.
4. Wait in the in-game lobby; both players **Ready** to start.

### Join (relay)

1. **Join** → enter display name.
2. Paste the host's **`SF4-XXXX`** code.
3. Wait until the host has clicked **Start game**, then click **Start game** on your side.
4. Press **Ready** in the lobby when connected.

Broker URL is preconfigured (`http://74.208.200.95:8787`). Override with `SF4E_BROKER_URL` or Advanced → **Room broker URL**.

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
```

## Playing over the internet (WAN)

| Requirement | Notes |
|-------------|--------|
| Same `Sidecar.dll` on both PCs | Join fails with "version mismatch" otherwise |
| Host port-forward **23456** | **Simple relay:** no host port forward. **Direct IP (Advanced):** host forwards session port (default 23456, TCP+UDP) |
| **WebView2 Runtime** | Launcher shows an install link if missing |
| Room code **`SF4-XXXX`** | Default in Simple mode; broker resolves to host relay |
| Direct **`IP:port`** | Advanced mode only |

**Relay mode (default):** GGPO rollback traffic is tunneled through the session connection. You usually do **not** need to forward separate GGPO UDP ports. Set `SF4E_RELAY=0` only for advanced LAN troubleshooting.

## Build ID (version check)

Both players need the same `Sidecar.dll` build. Enable developer overlay (`SF4E_NETPLAY_DEV=1`) to see the build hash in-game.

## Graphics

Disable **Smooth** frame rate in in-game graphics options if rollback feels wrong.

## Logs

`Launcher.exe --console` or files under `%APPDATA%\sf4e\`.

## Linux / Steam Deck

Use `protontricks-launch --appid 45760 Launcher.exe` as described in the main README.
