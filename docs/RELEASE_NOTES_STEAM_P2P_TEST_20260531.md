# SF4e Steam P2P experiment (Qt) — tester build 20260531

**Pre-release / experimental** — Steam Friends invite + SteamNetworkingSockets netplay path. Not the production VPS relay build on `main`.

> **Unofficial unsigned build.** Download only from this GitHub release page (Assets), not “Source code”.

## Install (both PCs)

1. Download **`sf4e-steam-p2p-test-20260531.zip`** from [this release](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/tag/steam-p2p-test-20260531).
2. Extract the **entire** zip to a short path (e.g. `C:\Games\sf4e-steam-p2p\`).
3. Run **`preflight.cmd`** — expect `[OK]` on required files and **`Preflight PASSED`**.
4. Optional smoke test: **`tools\run-offline-test.ps1`** (offline overlay; no Steam friend required).
5. **Steam must be running** on both PCs. Launch **`Launcher.exe`** (native Qt UI — no WebView2).

## Play (Steam P2P)

| Role | Steps |
|------|--------|
| **Host** | Send invite + listen → wait until P2P shows connected → **Start game** |
| **Join** | Accept invite + connect → **Start game** |

Full flow and diagnostics: `readme\STEAM_P2P_EXPERIMENT.md` inside the zip.

## Requirements

- Windows 10/11, **Ultra Street Fighter IV** (Steam), both players on **Steam friends**
- VC++ 2015–2022 x86 redistributable (preflight warns if missing)
- **GGPO rollback** in-game (same as relay builds)

## Build info

- Package: `sf4e-steam-p2p-experiment-qt`
- Built: `20260531-2119`
- Git: `29bf626` on branch `test/steam-p2p-qt`

## Bug reports

Include `readme\BUILD_INFO.txt`, both players’ `%APPDATA%\sf4e\logs\` (`launcher.log`, `sidecar_bootstrap.log`, `sf4e.log`), and whether preflight + offline test passed.

## Known limits

- In-app updater still targets production `sf4-netplay-launcher-*` assets — use this release zip for updates during the experiment.
- Two-PC Steam friend test recommended before wider rollout.
