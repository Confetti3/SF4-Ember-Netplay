# SF4 Netplay Launcher v0.2.9

Security hardening release — closes Critical update/URL handler issues and strengthens the room broker. **Update the launcher on every PC before playing** after the VPS broker is upgraded.

> **Not production-ready.** Experimental unofficial port for a small friends group.

## Security (launcher)

- **Auto-update** always re-fetches from GitHub; external download URLs are blocked
- **Release links** open only on HTTPS GitHub / GitHub Pages
- **Broker URL** blocks private/metadata addresses (SSRF protection)
- **WebView** navigation locked to local `launcher-ui/`; DevTools disabled in release builds
- Post-extract zip path validation before install

## Security (broker + VPS)

- **Host secret** required for room heartbeat/delete (prevents room griefing)
- **Longer room codes** (~64-bit entropy; format `SF4-` + 16 hex chars)
- Rate limits, request body cap, queue cap; room list disabled by default
- Relay dashboard login rate limiting

## Netplay changes

- New room codes are longer — share the full `SF4-…` code from the host screen
- Old short codes expire within ~15 minutes; create a new room after updating
- **Both players must use v0.2.9** after broker deploy (host secret required)

## Install / upgrade

1. Download **sf4-netplay-launcher-*.zip** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest)
2. Extract over your existing folder (or use **Check for updates** from v0.2.8.x)
3. Run **`Launcher.exe`** on both PCs before hosting

## Prerequisites

Unchanged — USF4 on Steam, WebView2 Runtime, VC++ x86 redistributable.


## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) � black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, and logs.

## Bug reports

Include `BUILD_INFO.txt` Git line, `%APPDATA%\sf4e\logs\sf4e.log`, and steps to reproduce.
