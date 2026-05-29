# SF4 Netplay Launcher v0.3.6

**Current release** — tested for VPS UDP GGPO netplay including rematches in the same room.

> **Experimental unofficial port** — unsigned build (Windows Defender may flag `Sidecar.dll`). Authenticode signing via SignPath is planned.

## Install

1. Download **`sf4-netplay-launcher-*-v0.3.6.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (Assets — not Source code).
2. Extract the **entire** zip on **both** PCs.
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.
4. Confirm both players show **v0.3.6** in the launcher header.

## What's in v0.3.6

- **Rematch UDP:** VPS GGPO relay re-binds player slots on re-registration (no silent drop when both slots were full).
- **Rematch transport:** broker UDP relay endpoint preserved after legacy fallback so later rematches still retry UDP.
- **UDP GGPO:** registration with local `ggpoPort`, `SF4W`/`SF4R` responses, health probe, overlay sync state, legacy fallback when relay is down.

## Netplay (VPS)

- Default broker: `https://74-208-200-95.nip.io` with `BROKER_GGPO_TRANSPORT=auto`
- After portraits, logs should show `GgpoTransport: UDP relay registration OK` and `GGPO: Running`

## Security

- No Defender exclusion scripts. Download only from this GitHub release page.


## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) � black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, and logs.

## Bug reports

Include `BUILD_INFO.txt`, room code, Network panel GGPO path, and `sf4e.log` from both PCs.
