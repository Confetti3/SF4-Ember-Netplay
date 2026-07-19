# SF4 Netplay Launcher v0.4.7

Production recovery release for matches stuck on **Waiting to sync...**, one-sided `SF4R`, and unreliable rematches.

## Install

1. Download **`sf4-netplay-launcher-*-0.4.7.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest).
2. Extract the full zip to a new folder.
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.
4. Confirm both players show **v0.4.7** and host a fresh room.

Both players must use the same release. Do not mix v0.4.7 with an older `Sidecar.dll`.

## Fixes

- Adds explicit player-slot and per-match-generation registration to the VPS UDP relay.
- Prevents registration keepalives from invalidating a peer that already received `SF4R`.
- Keeps the old match forwarding while both players register the next rematch, then switches atomically.
- Supports players behind the same public IP without IP-based slot guessing.
- Preserves the real NAT endpoint observed by the VPS relay.
- Releases any leftover GGPO socket before same-port registration.
- Validates relay response sources and times out every non-Running GGPO sync phase.
- Prevents overlapping relay-manager processes during idempotent starts and replacements.

## Verification

The deployed VPS passed the automated 17-check broker/relay suite, including:

- v0.4.6 compatibility
- repeated initial-registration probes
- registration-to-GGPO socket handoff
- bidirectional forwarding
- stale-generation isolation
- same-IP endpoint rebinding
- sequential rematch generations
- relay-manager idempotent start and clean replacement
