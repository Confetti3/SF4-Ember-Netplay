# SF4 Netplay Launcher v0.4.2

**Launcher UI fix release** — repairs Advanced mode, startup crash, and squished controls on smaller windows.

> **Experimental unofficial port** — unsigned build. Download only from this GitHub release page.

## Install

1. Download **`sf4-netplay-launcher-*-v0.4.2.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (Assets — not Source code).
2. Extract the **entire** zip on **both** PCs.
3. Run **`preflight.cmd`** — you should see `[OK]` for each file and `RESULT: PASS`.
4. Run **`Launcher.exe`**. Confirm both players show **v0.4.2** in the launcher header (or matching `BUILD_INFO.txt` Git line).

## What's in v0.4.2

- **Advanced mode fixed** — Simple/Advanced toggle now shows only the correct settings panels (no duplicate Host/Join forms).
- **Startup crash fixed** — launcher no longer exits immediately on open after switching to Advanced mode.
- **Resizable UI** — Home and Join screens scroll when the window is short; form fields keep readable minimum heights.
- **Join screen** — version-match hint visible in Advanced mode; display name uses the correct field on Join.
- **In-app updater** — use **Check for updates** on the home screen after install.

## Netplay (VPS)

Unchanged from v0.4.1 — same broker, room codes, and session flow:

- Default broker: `https://74-208-200-95.nip.io`
- Host → **Get code** → share `SF4-XXXX` → **Start game**
- Joiner → paste code → **Start game** after host starts
- Both **Ready** in the in-game lobby

## Troubleshooting

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## Upgrade from v0.4.1

Replace the **whole** extracted folder on both PCs. Do not copy only `Launcher.exe`. Both players must use the same zip.
