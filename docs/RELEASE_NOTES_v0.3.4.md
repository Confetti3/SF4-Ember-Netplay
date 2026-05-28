# SF4 Netplay Launcher v0.3.4 (planned)

**Target:** First **Authenticode-signed** release promoted to **Latest**, with v0.3.2 netplay fixes (UDP relay, connect-plan, GGPO path UI).

> **Not released yet.** Requires [SignPath Foundation](https://signpath.org/apply) approval and GitHub secrets configured (see `docs/CODE_SIGNING.md`).

## Prerequisites

Same as v0.3.2 — see [RELEASE_NOTES_v0.3.2.md](RELEASE_NOTES_v0.3.2.md).

## Signing

After `Release Windows` workflow on tag `v0.3.4`:

```powershell
Get-AuthenticodeSignature Launcher.exe, Sidecar.dll, RelayHost.exe, Updater.exe | Format-List
```

Publisher should show **SignPath Foundation** (status **Valid**).

## Windows Defender

Signed builds are the permanent mitigation for `Program:Win32/Wacapew.A!ml` on `Sidecar.dll`. Also submit binaries via `scripts/prepare-defender-submission.ps1` if needed.

## Install

1. Download **`sf4-netplay-launcher-*-v0.3.4.zip`** from GitHub Releases (Assets).
2. Extract fully → run **`preflight.cmd`** → **`Launcher.exe`**.

## What's fixed (UDP black screen)

- **GGPO UDP relay registration** now includes each player's local `ggpoPort` so the VPS relay forwards packets on the same ports GGPO uses (fixes black screen after portraits when overlay showed UDP relay).

## What's included (from main since v0.3.1)

- UDP GGPO relay transport (v0.3.2)
- In-game **GGPO path** display
- Windows VERSIONINFO on shipped binaries
- No Defender exclusion scripts
