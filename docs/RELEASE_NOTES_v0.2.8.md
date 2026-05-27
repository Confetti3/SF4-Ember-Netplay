# SF4 Netplay Launcher v0.2.8

Rebrand and security hardening release.

## What's new

- **Product rename:** SF4 Enhanced → **SF4 Netplay Launcher** ([Confetti3/SF4-Netplay-Launcher](https://github.com/Confetti3/SF4-Netplay-Launcher))
- **Upstream attribution:** README, launcher UI, and `ATTRIBUTION.md` clearly credit [sf4e by Anthony Danducci](https://codeberg.org/adanducci/sf4e) as the upstream project — this is an **unofficial port** (not maintained or endorsed by Anthony Danducci)
- **Safer in-app updater:** no PowerShell `-ExecutionPolicy Bypass` — updates use native `tar.exe` for zip extraction and **`Updater.exe`** to apply files
- **Preflight helper:** double-click **`preflight.cmd`** instead of running PowerShell with bypass flags

## Based on sf4e

This build is an unofficial port of [sf4e](https://codeberg.org/adanducci/sf4e) by **Anthony Danducci** and contributors (MIT). Anthony Danducci does not maintain or endorse this port. See `ATTRIBUTION.md` in the zip.

## Prerequisites

- **Ultra Street Fighter IV** on Steam (app 45760) — not included
- [Microsoft Edge WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
- [VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe)

## Install

1. Download the **sf4-netplay-launcher-*.zip** asset from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest)
2. Extract the entire zip to one folder
3. Optional: run **`preflight.cmd`**
4. Run **`Launcher.exe`**

Both players must use the **same release zip**.

## Simple mode (recommended)

Host → **Create relay room** → share **`SF4-XXXX`** → Joiner pastes code → both **Start game** → **Ready** in-game.

## Upgrade from v0.2.7.x

Use **Check for updates** on the launcher home screen, or download this zip manually on both PCs.

## Bug reports

Include the Git line from `BUILD_INFO.txt`, `%APPDATA%\sf4e\logs\sf4e.log`, and steps to reproduce. Update logs also write to `%TEMP%\sf4-netplay-update.log` when using the in-app updater.
