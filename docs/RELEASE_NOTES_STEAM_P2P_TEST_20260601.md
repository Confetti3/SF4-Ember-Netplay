# SF4e Steam P2P experiment (Qt) — tester build 20260601

**Pre-release / experimental** — Steam Friends invite + SteamNetworkingSockets netplay. Branch: `test/steam-p2p-qt`.

> **Unofficial unsigned build.** Download only from this GitHub release page (Assets), not “Source code”.

## What changed vs 20260531

- **Steam invite fixes** — wait for Steam relay before sending; auto-accept friend message sessions; clearer errors when invite send fails.
- **Joiner requirement called out** — joiner must have this launcher open on the **Join** tab before the host sends an invite (invites are not Steam chat messages).

## Install (both PCs)

1. Download **`sf4-netplay-p2p-steam-20260601.zip`** from [this release](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/tag/steam-p2p-test-20260601).
2. Extract the **entire** zip to a short path (e.g. `C:\Games\sf4-netplay-p2p-steam\`).
3. Run **`preflight.cmd`** — expect **`Preflight PASSED`**.
4. Optional: **`tools\run-offline-test.ps1`** for local overlay smoke test.
5. **Steam must be running** on both PCs. Launch **`Launcher.exe`**.

## Play (Steam P2P)

| Step | Who | Action |
|------|-----|--------|
| 1 | **Joiner** | Open launcher → **Join** tab → leave it open |
| 2 | **Host** | Select friend → **Send invite + listen** |
| 3 | **Joiner** | Activity log shows invite → **Accept invite + connect** |
| 4 | Both | Wait for **P2P connected** → **Start game** |

## Build info

- Package: `sf4-netplay-p2p-steam-qt`
- Git: `deeceb9` on `test/steam-p2p-qt`

## Bug reports

Include `readme\BUILD_INFO.txt`, `%APPDATA%\sf4e\logs\launcher.log` from both PCs, and whether joiner had the launcher open on Join before the host sent the invite.

## Known limits

- In-app updater still targets production `sf4-netplay-launcher-*` zips — use this release for updates during the experiment.
