# SF4 Netplay Launcher v0.4.6

Hotfix for matches stuck on **Waiting to sync…** after Ready (both players got `SF4R` but GGPO never connected).

## Install

1. Download **`sf4-netplay-launcher-*-0.4.6.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (Assets — not Source code).
2. Extract the **full** zip to a new folder.
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.
4. Confirm both players show **v0.4.6**. Host a **new** room (do not reuse an old room from before the VPS update).

## What's in v0.4.6

- **UDP relay registration binds the local GGPO port** so NAT hole-punching matches the socket GGPO uses after Ready.
- **VPS `sf4e-ggpo-udp-relay` stores the real NAT endpoint** from registration instead of overwriting with declared port `23457` (which black-holed GGPO traffic after a successful `SF4R`).

## Notes

- Both players must use this zip (`Sidecar.dll` must match).
- VPS relay binaries are updated with this release; new rooms pick up the fix automatically after deploy.
