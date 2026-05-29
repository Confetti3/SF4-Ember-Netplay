# SF4 Netplay Launcher v0.3.7

**Test build** — launcher keepalive for VPS rooms + broker tiered idle (VPS already updated).

> **Experimental unofficial port** — unsigned build. Download only from this GitHub release page.

## Install

1. Download **`sf4-netplay-launcher-*-v0.3.7.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (Assets — not Source code).
2. Extract the **entire** zip on **both** PCs.
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.
4. Confirm both players show **v0.3.7** in the launcher header.

## What's in v0.3.7

- **Host keepalive:** relay room heartbeat continues after **Start game** (every ~3 min) so long matches are less likely to lose the VPS room slot.
- **VPS broker (server-side):** abandoned host-only codes expire in ~**5 min**; rooms with a joiner or active match stay up to ~**30 min** (no launcher change required on joiner).
- Includes all **v0.3.6** rematch/UDP relay fixes — both players must use the **same** zip.

## Netplay (VPS)

- Default broker: `https://74-208-200-95.nip.io`
- Simple mode: Host → share `SF4-XXXX` → both Start game → Ready


## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) � black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, and logs.

## Bug reports

Include `BUILD_INFO.txt`, room code, bottom overlay RTT/LFB/RFB peaks, and `sf4e.log` from both PCs.
