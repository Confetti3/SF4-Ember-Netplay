# SF4 Netplay Launcher v0.3.4 (testing)

**Pre-release for testing** — not promoted to Latest. **v0.3.1** remains the default stable download until Authenticode signing ships.

> **Experimental unofficial port** — friends-only testing. **Unsigned** build (Defender may flag `Sidecar.dll`).

## Prerequisites

Same as v0.3.2 — see [RELEASE_NOTES_v0.3.2.md](RELEASE_NOTES_v0.3.2.md).

## Install

1. Download **`sf4-netplay-launcher-*-v0.3.4.zip`** (Assets — not Source code). Re-download if you had an earlier v0.3.4 zip from before this rebuild.
2. Extract the **entire** zip on **both** PCs (overwrite the old folder).
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.
4. Confirm both players show **v0.3.4** in the launcher header and check **`BUILD_INFO.txt`** Git line matches the latest release.

## What's fixed in v0.3.4

- **UDP GGPO black screen after portraits:** registration sends each player's local `ggpoPort`; relay accepts **`SF4W`** (waiting) and **`SF4R`** (ready).
- **UDP relay health:** probes relay with `SF4H` before registration; falls back to legacy tunnel if the VPS GGPO relay is down (both sides stay on the same path).
- **VPS broker:** restarts GGPO UDP relay on heartbeat and connect-plan; connect-plan downgrades to legacy if GGPO relay cannot start.
- **Sync safety:** no legacy-tunnel restart while GGPO is already connected/synchronizing.
- **Overlay:** shows GGPO sync state (`Waiting for sync…` / `Synchronizing…`) before **In match**.
- **Fallback:** if UDP GGPO does not reach Running within ~3s (and sync has not started), auto-fallback to legacy session tunnel.

## Netplay (VPS Simple mode)

- Broker: default `https://74-208-200-95.nip.io` with `BROKER_GGPO_TRANSPORT=auto`
- Network panel should show **GGPO path: UDP relay**, then **`GGPO: Running`** in logs after portraits

## Verify (`%APPDATA%\sf4e\logs\sf4e.log`)

```
GgpoTransport: UDP relay registration OK ... localGgpoPort=23457
GGPO: Connected!
GGPO: Synchronizing: ...
GGPO: Running
```

## Security note

- No Defender exclusion scripts in this build.
- Download only from this GitHub release page; verify SHA256 on the release assets if Defender blocks files.

## Bug reports

Include `BUILD_INFO.txt` Git line, room code, **GGPO path** from Network panel, and `sf4e.log` from both PCs.
