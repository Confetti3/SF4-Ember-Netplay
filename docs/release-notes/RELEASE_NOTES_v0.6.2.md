# SF4 Netplay Launcher v0.6.2 (testing)

> **Experimental unofficial test build** — not production-ready software.
> Both players must install the same complete release zip.

This release fixes the Direct-IP GGPO handshake regression, removes the
30-minute age limit for occupied VPS rooms, and expands the default VPS relay
capacity to 50 concurrent rooms.

## Install

1. Download `sf4-netplay-launcher-*-0.6.2.zip` from this release.
2. Extract the complete zip into a new folder.
3. Run `preflight.cmd`, then `Launcher.exe`.
4. Confirm both players show **v0.6.2**.

Mixed builds are rejected by the room compatibility check.

## Fixes and changes

- Fixed Direct IP reaching `GGPO: session started` but never reaching
  `Connected` or `Synchronized`. The GGPO startup path no longer destroys the
  legacy session tunnel immediately after it is created.
- Occupied or started VPS rooms no longer expire after 30 minutes by default.
  Empty host-only rooms still expire after five minutes so abandoned room
  codes do not consume relay capacity.
- Operators can restore an occupied-room idle limit by setting
  `ROOM_OCCUPIED_IDLE_MS` to a positive millisecond value; `0` disables it.
- Raised the default VPS capacity from 20 to 50 rooms, with session ports
  `23456-23505` and GGPO UDP relay ports `24456-24505`.
- Updated broker health, dashboard display, deployment scripts, firewall
  ranges, and capacity/idle-policy regression coverage.
- Includes all rollback diagnostics, pacing, lifecycle, and training fixes
  from v0.6.1.

## Direct-IP requirements

- Host selects **Advanced → Direct IP** and starts the game first.
- Host forwards the session port (default `23456`) on TCP and UDP and permits
  it through Windows Firewall.
- Joiner uses the host's public or VPN `IP:port`.
- Both players press Start or LK when prompted to bind their controller.

## Validation

- Broker and dashboard JavaScript syntax checks.
- Broker capacity and idle-policy tests.
- Native x86 RelWithDebInfo Sidecar build.

Live two-PC Direct-IP play and rematch remain recommended release-candidate
checks because they require two separate game installations and networks.
