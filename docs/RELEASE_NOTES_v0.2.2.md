# SF4 Netplay Launcher v0.2.2

Rollback netplay for **Ultra Street Fighter IV** (Steam) with launcher-driven Host / Join / Offline.

## What's new

- **UI polish:** copy feedback and status messages no longer overlap the header or share cards; single toast on copy
- **Launcher stability:** loading gate until first state sync, cleaner status strip, relay room success path fixes
- **Relay errors:** clearer broker messages for timeout, connection refused, and HTTP 503 (includes broker URL)
- **Relay persist:** successful create-relay saves room code and broker settings to `%APPDATA%\sf4e\config.json`
- **New room broker:** default `http://74.208.200.95:8787`; auto-migrates old Oracle broker URL on launcher start
- **In-app updates:** **Check for updates** / **Install update** on the home screen (GitHub Releases)

## Based on sf4e

Unofficial port of [sf4e](https://codeberg.org/adanducci/sf4e) by Anthony Danducci (MIT). Anthony Danducci does not maintain or endorse this port. See `ATTRIBUTION.md` in the zip.

## Prerequisites

- **Ultra Street Fighter IV** on Steam (app 45760)
- [Microsoft Edge WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
- [VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe)

## Install

1. Download the **team zip** asset below (not "Source code" only).
2. Extract the **entire** zip to one folder.
3. Run `preflight.cmd`
4. Run `Launcher.exe` — **Host**, **Join**, or **Offline**

## Quick start (relay)

| Host | Joiner |
|------|--------|
| **Create relay room** → share **`SF4-XXXX`** | Paste **`SF4-XXXX`** |
| **Start game** (starts `RelayHost.exe`) | **Start game** |
| Forward **TCP+UDP** on assigned port (or UPnP in Advanced) | No port forward needed |
| Both **Ready** in-game | Same zip on both PCs |

**Direct IP:** Advanced mode → share LAN / Internet address (`IP:23456`).

## Upgrade from v0.2.1

If relay create fails or joiners can't find your room, check Advanced → **Room broker URL** is `http://74.208.200.95:8787`. The launcher auto-migrates the old Oracle URL; you can also delete `%APPDATA%\sf4e\config.json` to reset defaults.

Both players must use the **same release zip** (`Sidecar.dll` must match).

## Support

Include the **Git** line from `BUILD_INFO.txt` when reporting issues.
