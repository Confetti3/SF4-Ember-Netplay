# SF4 Netplay Launcher v0.2.8.1

Experimental framing release — same netplay build as v0.2.8 with updated messaging in the launcher UI, docs, and package metadata.

> **Not production-ready.** Experimental unofficial port for a small friends group — not presented as stable or finished netplay software.

## What's new

- **Experimental framing** across README, launcher UI, `START_HERE.md`, tester docs, `BUILD_INFO.txt`, and window title
- Clear **not production-ready** / friends-only test software messaging (per upstream feedback)
- **Unofficial port** disclaimers unchanged from v0.2.8 — not maintained or endorsed by Anthony Danducci

## Unofficial port (not official sf4e)

This build is an unofficial port of [sf4e](https://codeberg.org/adanducci/sf4e) by **Anthony Danducci** and contributors (MIT). See `ATTRIBUTION.md` in the zip.

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

## Simple mode (experimental)

Host → **Create relay room** → share **`SF4-XXXX`** → Joiner pastes code → both **Start game** → **Ready** in-game. This path has worked in limited testing but **may still fail**.

## Upgrade from v0.2.8 or v0.2.7.x

Use **Check for updates** on the launcher home screen, or download this zip manually on both PCs.

## Bug reports

Include the Git line from `BUILD_INFO.txt`, `%APPDATA%\sf4e\logs\sf4e.log`, and steps to reproduce.
