# SF4 Netplay Launcher v0.3.3

Defender false-positive guidance + install helper. **Binaries are still unsigned** — see below.

> **Not production-ready.** Experimental unofficial port for a small friends group.

## Prerequisites

Same as v0.3.2 — see [RELEASE_NOTES_v0.3.2.md](RELEASE_NOTES_v0.3.2.md).

## Windows Defender (`Wacapew.A!ml`) — read this first

`Sidecar.dll` may be flagged when downloading or extracting. This is a **known false positive** on unsigned game-hook DLLs.

**Your friend should:**

1. Download from [GitHub Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) only.
2. **Allow on device** in the Defender alert, **or**
3. Run **`defender-add-exclusion.ps1`** as Administrator (included in zip):

```powershell
powershell -ExecutionPolicy Bypass -File defender-add-exclusion.ps1 -InstallDir "C:\Games\SF4-Netplay-Launcher"
```

Full details: `docs/WINDOWS_DEFENDER.md` and `docs/CODE_SIGNING.md`.

**Permanent fix in progress:** [SignPath Foundation](https://signpath.org/apply) free OSS code signing (see `.signpath/signpath.json` in repo).

## What's in v0.3.3

- `defender-add-exclusion.ps1` in the zip (folder exclusion helper)
- Updated `docs/WINDOWS_DEFENDER.md`, `docs/CODE_SIGNING.md`
- Windows **VERSIONINFO** on `Launcher.exe`, `Sidecar.dll`, `RelayHost.exe`, `Updater.exe`
- v0.3.2 netplay fixes unchanged (UDP relay, GGPO path UI)

## Install

1. Download **`sf4-netplay-launcher-*-v0.3.3.zip`** (Assets — not Source code).
2. Extract fully → run **`preflight.cmd`** → **`Launcher.exe`**.
