# SF4e Steam P2P experiment (Qt) — tester build 20260601

**Pre-release / experimental** — Steam Friends invite + SteamNetworkingSockets netplay. Branch: `test/steam-p2p-qt`.

> **Unofficial unsigned build.** Download only from this GitHub release page (Assets), not “Source code”.

## What changed vs 20260531

- **Steam invite fixes** — wait for Steam relay before sending; auto-accept friend message sessions; clearer errors when invite send fails.
- **Synchronized launch** — both players press **Ready to launch game**; USF4 starts on both PCs only after the handshake completes (fixes solo-host crash).
- **Launch handshake hardening** — resend launch-ready, faster polling while waiting, recovery if launch fails or one player is stuck.
- **In-game lobby wait** — match/GGPO does not start until two players are in the lobby.
- **Synchronized launch fix** — launch-ready messages carry the invite session token; stale signals are drained; both PCs must confirm before `steamStart`.
- **Launch commit barrier** — after both players are launch-ready, both exchange `launch_commit` for the same session token; neither PC calls `steamStart` until peer commit is confirmed (fixes split launch when one side closes P2P first).
- **Launch handshake vs P2P socket** — mark launch-ready/commit while Steam is initialized and the session token is valid, even if the P2P socket shows disconnected; only the listen/connect socket is closed at game start, not Networking Messages.
- **GGPO match-start fix** — Steam P2P keeps rollback over the session tunnel (`GgpoRelay`); no invalid Steam-address fallback at character select / Start.
- **Steam API reuse in-game** — avoids second `SteamAPI_Init()` when USF4 already loaded Steam.
- **Joiner requirement called out** — joiner must have this launcher open on the **Join** tab before the host sends an invite (invites are not Steam chat messages).

## Install (both PCs)

1. Download **`sf4-netplay-p2p-steam-20260601-2209.zip`** from [this release](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/tag/steam-p2p-test-20260601) (not older `2155` / `2143` / `20260531` assets).
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
| 4 | Both | Wait for **P2P connected** → **Ready to launch game** (both must click; wait for **Both committed — launching**) |
| 5 | Both | In-game lobby → pick characters → both **Ready** → play |

## Build info

- Package: `sf4-netplay-p2p-steam-qt`
- Built: `20260601-2209`
- Git: `test/steam-p2p-qt` (launch-ready + launch-commit barrier)

## Bug reports

Include `readme\BUILD_INFO.txt`, `%APPDATA%\sf4e\logs\launcher.log` from both PCs, and whether joiner had the launcher open on Join before the host sent the invite.

## Known limits

- In-app updater still targets production `sf4-netplay-launcher-*` zips — use this release for updates during the experiment.
