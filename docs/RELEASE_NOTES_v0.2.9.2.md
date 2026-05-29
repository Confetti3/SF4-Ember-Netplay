# SF4 Netplay Launcher v0.2.9.2

Hotfix for v0.2.9.1 — fixes question-mark (`?`) characters in launcher UI and in-game netplay overlay.

> **Not production-ready.** Experimental unofficial port for a small friends group.

## Bug fixes

- **Launcher UI:** Replace Unicode punctuation (em dash, ellipsis) with ASCII so WebView2 no longer shows `?` in labels, hints, and empty share fields
- **Launcher icons:** Add `xlink:href` on SVG `<use>` elements for reliable icon rendering in WebView2
- **In-game netplay overlay (Sidecar):** Same typography fix for ImGui strings (room code line, lobby hint, version mismatch alert)

## Upgrade

1. Download **sf4-netplay-launcher-*.zip** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest)
2. Extract over your existing folder (or use **Check for updates** from v0.2.9 / v0.2.9.1)
3. Replace **Sidecar.dll** and **launcher-ui/** — both are included in the zip

No VPS/broker redeploy needed.


## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) � black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, and logs.

## Bug reports

Include `BUILD_INFO.txt` Git line, `%APPDATA%\sf4e\logs\sf4e.log`, and steps to reproduce.
