# Windows Defender and antivirus (false positives)

## Is this malware?

**No.** `Program:Win32/Wacapew.A!ml` on **`Sidecar.dll`** is a **heuristic false positive**, not confirmed malware.

The **`!ml`** suffix means Defender used **machine learning**, not a known virus signature. **Unsigned** game-hook DLLs are flagged until they are **Authenticode-signed** or Microsoft clears the detection.

## Fastest fix for your friend (unsigned build)

**Option 1 — Allow in the alert**

1. Download from [GitHub Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) only.
2. Extract the zip.
3. When Defender blocks **`Sidecar.dll`**: **Allow on device** (or Restore → Allow).

**Option 2 — Folder exclusion (recommended for repeat play)**

Run **PowerShell as Administrator** from the install folder:

```powershell
powershell -ExecutionPolicy Bypass -File defender-add-exclusion.ps1 -InstallDir "C:\Games\SF4-Netplay-Launcher"
```

(Change the path to wherever you extracted the zip.)

Then re-extract or restore quarantined files and run `Launcher.exe`.

## Permanent fix (maintainer — signed builds)

Unsigned builds will keep triggering `Wacapew.A!ml` on some PCs. The real fix is **code signing**:

- Apply for free OSS signing: [SignPath Foundation](https://signpath.org/apply) — see [`docs/CODE_SIGNING.md`](CODE_SIGNING.md)
- Or use [Azure Artifact Signing](https://learn.microsoft.com/en-us/azure/artifact-signing/overview) in GitHub Actions

Signed releases show publisher **SignPath Foundation** or your certificate and build SmartScreen/Defender trust over time.

## Why Defender flags this project

| Behavior | Why we do it | Why AV cares |
|----------|--------------|--------------|
| **`Sidecar.dll` injected into USF4** | Rollback netplay (GGPO) | Same pattern as cheats/trainers |
| **Microsoft Detours** | Official hook library | Process modification |
| **Networking** | Online play | Extra scrutiny |
| **No signature (yet)** | Indie OSS | Low reputation score |

Source: [github.com/Confetti3/SF4-Netplay-Launcher](https://github.com/Confetti3/SF4-Netplay-Launcher)

## Verify files (optional)

```powershell
Get-FileHash Launcher.exe, Sidecar.dll, RelayHost.exe -Algorithm SHA256 | Format-Table
```

Compare with hashes on the GitHub release page.

## Maintainer checklist each release

1. Run `scripts/prepare-defender-submission.ps1` and submit the three binaries at [Microsoft file submission](https://www.microsoft.com/en-us/wdsi/filesubmission) (**Incorrectly detected** → `Wacapew.A!ml`).
2. Ship **signed** binaries when SignPath/Azure is configured (`docs/CODE_SIGNING.md`).
3. Post SHA256 hashes in release notes.
