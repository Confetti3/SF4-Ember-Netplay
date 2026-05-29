# SF4 Netplay Launcher v0.3.8

**Update** — fixes `preflight.cmd`, ships the player troubleshooting guide in the zip, and refreshes packaged docs.

> **Experimental unofficial port** — unsigned build. Download only from this GitHub release page.

## Install

1. Download **`sf4-netplay-launcher-*-v0.3.8.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (Assets — not Source code).
2. Extract the **entire** zip on **both** PCs.
3. Run **`preflight.cmd`** — you should see `[OK]` for each file and `RESULT: PASS` (or warnings for WebView2/VC++ if not installed).
4. Run **`Launcher.exe`**. Confirm both players show **v0.3.8** in the launcher header (or matching `BUILD_INFO.txt` Git line).

## What's in v0.3.8

- **`preflight.cmd` fixed** — works when double-clicked; finds `preflight.ps1` in the zip; passes the correct package folder; pauses on failure so errors are visible.
- **`docs/TROUBLESHOOTING.md`** included in the zip — black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, log collection.
- **Packaged docs** updated (`START_HERE.md`, `USER_NETPLAY.md`, `BETA_TESTERS.md`, etc.).
- **Same binaries as v0.3.7** (keepalive + tiered idle VPS broker) — no gameplay code changes in this build.

## Netplay (VPS)

- Default broker: `https://74-208-200-95.nip.io`
- Simple mode: Host → share `SF4-XXXX` → both **Start game** → **Ready**

## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) — also at `docs/TROUBLESHOOTING.md` inside the zip.

## Bug reports

Include `BUILD_INFO.txt`, room code, bottom overlay RTT/LFB/RFB peaks, and `sf4e.log` from both PCs.
