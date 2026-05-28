# Defender binary comparison (v0.3.1 vs v0.3.3)

Verified **2026-05-27** from official GitHub release zips.

## Summary

| Finding | Detail |
|---------|--------|
| `src/sidecar/sidecar.cxx` | **No changes** between tag `v0.3.1` and `HEAD` |
| `src/launcher/launcher.cxx` | **No changes** between tag `v0.3.1` and `HEAD` |
| PE binaries | **Different SHA256** (VERSIONINFO / resource section added in v0.3.3) |
| Hook mechanism | **Same** — Microsoft Detours injection in `Sidecar.dll` |

Different file hashes do **not** mean v0.3.3 added malware behavior. They reflect rebuild + embedded version metadata. Defender’s `Program:Win32/Wacapew.A!ml` targets the **unsigned hook pattern**, which exists in both builds.

## SHA256 (release zips)

| File | v0.3.1 | v0.3.3 |
|------|--------|--------|
| `Sidecar.dll` | `CD58277BC8FA15254EE727CF04AFCECE0BD324EF55E87E0BCB74E97637BA21E9` | `64712952F257FE88C9647D4729B88274DBFF5D827C240F70D166ECFF13E48965` |
| `Sidecar.dll` size | 2,075,648 bytes | 2,096,128 bytes |
| `Launcher.exe` | `D0CD40391DD6C2DEF081F756CA2E3A0B5D0807F33505CA1A788AF56D1834851B` (2,068,992 B) | `BF30D9805838C4CF495BA92E807F19C821AFBD0ACE5E7D098665FC6AC19BF02F` (2,095,616 B) |

Reproduce locally:

```powershell
gh release download v0.3.1 -D dist\v031 --pattern "*.zip"
gh release download v0.3.3 -D dist\v033 --pattern "*.zip"
# Extract, then:
Get-FileHash .\path\to\Sidecar.dll -Algorithm SHA256
```

## What changed in v0.3.3 (not injection)

- [`cmake/sf4e_version.cmake`](../cmake/sf4e_version.cmake) — Windows VERSIONINFO on `Launcher.exe`, `Sidecar.dll`, `RelayHost.exe`, `Updater.exe`
- Netplay transport / overlay (v0.3.2 lineage) — does not modify Detours entry in [`src/sidecar/sidecar.cxx`](../src/sidecar/sidecar.cxx)

## Secure mitigation

1. **Authenticode signing** — SignPath Foundation ([`docs/CODE_SIGNING.md`](CODE_SIGNING.md), [`docs/SIGNPATH_APPLY.md`](SIGNPATH_APPLY.md))
2. **Microsoft WDSI submission** — `scripts/prepare-defender-submission.ps1`
3. **Do not** ship Defender folder-exclusion scripts (removed from `main`)

Public **Latest** release should remain **v0.3.1** until signed **v0.3.4+** ships.
