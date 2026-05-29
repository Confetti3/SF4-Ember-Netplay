# Windows Defender and antivirus (false positives)

## Is this malware?

**No.** `Program:Win32/Wacapew.A!ml` on **`Sidecar.dll`** is a **heuristic false positive**, not confirmed malware.

The **`!ml`** suffix means Defender used **machine learning**, not a known virus signature. **Unsigned** game-hook DLLs are flagged until they are **Authenticode-signed** or Microsoft clears the detection.

We do **not** recommend weakening Windows Defender (folder exclusions, disabling real-time protection, etc.). The durable fix is **signed releases** from this project.

## What to do if Defender blocks install (unsigned build)

1. Download only from [GitHub Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (currently **v0.3.1** until a signed build ships).
2. Compare SHA256 hashes with the release page (see below).
3. If you believe the detection is wrong, report it to Microsoft at [file submission](https://www.microsoft.com/en-us/wdsi/filesubmission) (**Incorrectly detected as malware** → `Program:Win32/Wacapew.A!ml`).
4. Wait for a **signed** release (see [`docs/CODE_SIGNING.md`](CODE_SIGNING.md)) — that is what we ship as the permanent fix.

If Defender offers **Allow on device** for a file you downloaded from our official release page and verified by hash, that is your local decision. We do not ship scripts or instructions to add Defender exclusions.

## Permanent fix (signed builds)

Unsigned builds may trigger `Wacapew.A!ml` on some PCs. **Authenticode signing** is the reliable fix:

- [SignPath Foundation](https://signpath.org/apply) (free for OSS) — see [`docs/CODE_SIGNING.md`](CODE_SIGNING.md)
- Or [Azure Artifact Signing](https://learn.microsoft.com/en-us/azure/artifact-signing/overview) in GitHub Actions

Signed releases show a verified publisher and build SmartScreen/Defender trust over time.

## Why Defender flags this project

| Behavior | Why we do it | Why AV cares |
|----------|--------------|--------------|
| **`Sidecar.dll` injected into USF4** | Rollback netplay (GGPO) | Same pattern as cheats/trainers |
| **Microsoft Detours** | Official hook library | Process modification |
| **Networking** | Online play | Extra scrutiny |
| **No signature (yet)** | Indie OSS | Low reputation score |

The injection code in [`src/sidecar/sidecar.cxx`](../src/sidecar/sidecar.cxx) is unchanged since **v0.3.1**; newer versions added netplay transport and PE version metadata, not a different hook mechanism. Different release builds can have different `Sidecar.dll` SHA256 hashes from rebuilds and VERSIONINFO — that does not mean the hook behavior changed.

Source: [github.com/Confetti3/SF4-Netplay-Launcher](https://github.com/Confetti3/SF4-Netplay-Launcher)

## Verify files (recommended)

```powershell
Get-FileHash Launcher.exe, Sidecar.dll, RelayHost.exe -Algorithm SHA256 | Format-Table
```

Compare with hashes on the GitHub release page.

## Maintainer checklist each release

1. Run `scripts/prepare-defender-submission.ps1` and submit `Launcher.exe`, `Sidecar.dll`, and `RelayHost.exe` at [Microsoft file submission](https://www.microsoft.com/en-us/wdsi/filesubmission) (**Incorrectly detected** → `Wacapew.A!ml`).
2. Ship **signed** binaries when SignPath/Azure is configured (`docs/CODE_SIGNING.md`).
3. Post SHA256 hashes in release notes.
4. Promote a release to **Latest** only after Authenticode signatures validate.
