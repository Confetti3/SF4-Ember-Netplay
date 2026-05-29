# SF4 Netplay Launcher v0.3.3

**Withdrawn from Latest — use [v0.3.1](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/tag/v0.3.1)** until Authenticode-signed **v0.3.4+** ships.

> **Not production-ready.** Experimental unofficial port for a small friends group.

## Prerequisites

Same as v0.3.2 — see [RELEASE_NOTES_v0.3.2.md](RELEASE_NOTES_v0.3.2.md).

## Windows Defender (`Wacapew.A!ml`)

**Pre-release.** `Sidecar.dll` may be flagged (`Program:Win32/Wacapew.A!ml`) — heuristic false positive on unsigned Detours hook, not confirmed malware.

- Download only from official GitHub Releases
- Verify SHA256 hashes on the release page
- See `docs/WINDOWS_DEFENDER.md` — **we do not ship or recommend Defender exclusion scripts** (removed from source after this release)
- Permanent fix: [SignPath Foundation](https://signpath.org/apply) signing — see `docs/CODE_SIGNING.md`

## What's in v0.3.3 (historical)

- Windows **VERSIONINFO** on `Launcher.exe`, `Sidecar.dll`, `RelayHost.exe`, `Updater.exe`
- Defender documentation (superseded — exclusion helper removed on `main`)
- v0.3.2 netplay fixes unchanged (UDP relay, GGPO path UI)

## Install

Do **not** use this build for general distribution. Install **[v0.3.1](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/tag/v0.3.1)** instead.

## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) � black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, and logs.
