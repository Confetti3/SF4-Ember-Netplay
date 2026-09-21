# SF4 Netplay Launcher v0.6.6-rc1 (testing)

> **Experimental unofficial test build** — not production-ready software.
> Both players must install the same complete release zip.

Security-hardening RC on top of **v0.6.5** Qt + HTTPS Simple mode. Happy-path Host / Join / VPS relay is unchanged for players; no new launcher settings are required.

## Install

1. Download `sf4-netplay-launcher-*-0.6.6-rc1.zip` from this release.
2. Extract the complete zip into a new folder.
3. Run `preflight.cmd`, then `Launcher.exe`.
4. Confirm both players show **v0.6.6**.

Mixed builds are rejected by the room compatibility check.

## What's new (hardening)

- Session server: post-HELLO membership gate (only join allowed until lobby membership), FORWARD fraud drop, REPORTRESULTS bounds, cidMap cleanup on disconnect
- Broker: rate limits on queue join and room health; optional `RELAY_MANAGER_TOKEN` shared with relay-manager (stored on operator PC / VPS `.env`, not in git)
- Relay-manager: session/GGPO port allowlists (default 50-room ranges); optional token auth via broker `.env`
- Launcher: broker DNS private-IP block (with Winsock init); update downloads re-check redirect hosts (`*.githubusercontent.com`)
- Dashboard: timing-safe cookie compare and JSON body size limit

## Unofficial port (not official sf4e)

This is an **unofficial port** of [sf4e](https://codeberg.org/adanducci/sf4e) by **Anthony Danducci** and contributors (MIT). It is **not** the upstream project and is **not** endorsed by Anthony Danducci. See `ATTRIBUTION.md` in the zip.

## Prerequisites

| Requirement | Download |
|-------------|----------|
| Ultra Street Fighter IV (Steam) | [Steam](https://store.steampowered.com/app/45760/) |
| VC++ Redistributable (x86) | [VC++ x86](https://aka.ms/vs/17/release/vc_redist.x86.exe) |

## Troubleshooting

See [docs/guides/TROUBLESHOOTING.md](../guides/TROUBLESHOOTING.md).
