# SF4 Netplay Launcher v0.2.9.1

Hotfix for v0.2.9 — fixes blank launcher window and relay code UX.

> **Not production-ready.** Experimental unofficial port for a small friends group.

## Bug fixes

- **Blank launcher window:** WebView navigation lockdown now correctly parses `file:///` URLs (v0.2.9 blocked the UI from loading)
- **Stale relay codes:** Host screen no longer restores old `SF4-…` codes from config on startup

## UX changes

- Relay room codes are **session-only** — click **Get code** each time you host
- **Start game** is blocked until you have a code from **Get code** (no silent auto-create)
- Button renamed from "Create relay room" to **Get code**

## Upgrade from v0.2.9

1. Download **sf4-netplay-launcher-*.zip** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest)
2. Extract over your existing folder (or use **Check for updates** from v0.2.9)
3. On host: click **Get code** before **Start game**

## Unchanged from v0.2.9

Security hardening, longer room codes, broker host-secret auth, and VPS relay behavior are unchanged. No broker redeploy needed.


## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) � black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, and logs.

## Bug reports

Include `BUILD_INFO.txt` Git line, `%APPDATA%\sf4e\logs\sf4e.log`, and steps to reproduce.
