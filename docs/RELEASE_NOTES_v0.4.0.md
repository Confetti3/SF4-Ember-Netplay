# SF4 Netplay Launcher v0.4.0

**Major update** — native Qt launcher UI with VPS relay netplay (`SF4-XXXX` room codes). Removes the Steam P2P experiment.

> **Experimental unofficial port** — unsigned build. Download only from this GitHub release page.

## Install

1. Download **`sf4-netplay-launcher-*-v0.4.0.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (Assets — not Source code).
2. Extract the **entire** zip on **both** PCs.
3. Run **`preflight.cmd`** — you should see `[OK]` for each file and `RESULT: PASS` (or a VC++ warning if not installed).
4. Run **`Launcher.exe`**. Confirm both players show **v0.4.0** in the launcher header (or matching `BUILD_INFO.txt` Git line).

## What's in v0.4.0

- **Qt Widgets launcher** — Simple/Advanced home screen, Host / Join / Offline, relay room codes, updates (Qt DLLs included; no WebView2).
- **In-game overlay** — same Network lobby as v0.3.8 (relay status, GGPO path, Ready flow).
- **VPS relay unchanged** — same broker (`https://74-208-200-95.nip.io`), `SF4-XXXX` room codes, connect-plan transport ladder.
- **Steam P2P experiment removed** — all Steamworks launcher/session code stripped from this release.
- **`NetplayConfig` v7** — both players must use v0.4.0 `Sidecar.dll` (breaking change from v0.3.x).

## Netplay (VPS)

- Default broker: `https://74-208-200-95.nip.io`
- Host → **Get code** → share `SF4-XXXX` → **Start game**
- Joiner → paste code → **Start game** after host starts
- Both **Ready** in the in-game lobby

## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) — also at `docs/TROUBLESHOOTING.md` inside the zip.

## Bug reports

Include `BUILD_INFO.txt`, room code, bottom overlay RTT/LFB/RFB peaks, and `sf4e.log` from both PCs.
