# SF4 Netplay Launcher v0.2.1

Rollback netplay for **Ultra Street Fighter IV** (Steam) with launcher-driven Host / Join / Offline.

## What's new

- **Controller fix:** launcher auto-netplay now waits for **Start or LK** pad capture before starting a session (matches upstream sf4e behavior; fixes keyboard-default / sporadic controller input)
- **Rematch:** after a match ends, lobby ready state resets automatically — both players press **Ready** again to run back without reconnecting (character/stage selections preserved)
- **Direct IP default:** Advanced mode and **Direct IP** are now the default connection method (relay room codes still available in the connection dropdown or Simple mode)

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

## Quick start (direct IP — default)

| Host | Joiner |
|------|--------|
| Advanced mode → note **LAN** / **Internet address** (`IP:23456`) | Paste host **`IP:23456`** |
| Forward **TCP+UDP 23456** (or try UPnP) | No port forward needed on joiner |
| Press **Start or LK** when prompted in-game | Same |
| Both **Ready** in lobby | Same zip on both PCs |

**Relay:** enable **Simple mode** or choose **Relay room** in the connection dropdown.

## Config note

If you upgraded from v0.2.0, delete or edit `%APPDATA%\sf4e\config.json` to pick up new defaults (`defaultConnectMethod`, `simpleUi`).

## Support

Include the **Git** line from `BUILD_INFO.txt` when reporting issues.
