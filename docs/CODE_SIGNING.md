# Code signing (fix Windows Defender false positives)

`Program:Win32/Wacapew.A!ml` on **`Sidecar.dll`** is a **heuristic** flag. Unsigned game-hook DLLs are routinely misclassified. **Authenticode signing** is the reliable fix.

We do **not** ship Defender folder-exclusion scripts. Do not ask users to run `Add-MpPreference` or disable scanning.

## Option A — SignPath Foundation (free for OSS, recommended)

1. Apply: [signpath.org/apply](https://signpath.org/apply)
2. Repo policy: [`.signpath/signpath.json`](../.signpath/signpath.json)
3. Requirements: MIT license, public GitHub, active maintenance, signing policy in repo
4. After approval: add GitHub secrets (see below) and run **Release Windows** on tag push; binaries are signed as **SignPath Foundation**

### SignPath GitHub secrets (after approval)

| Secret | Purpose |
|--------|---------|
| `SIGNPATH_API_TOKEN` | SignPath.io API token |
| `SIGNPATH_ORGANIZATION_ID` | Your SignPath organization GUID |
| `SIGNPATH_SIGNING_POLICY_SLUG` | e.g. `release` (matches `.signpath/signpath.json`) |

The workflow [`.github/workflows/release-windows.yml`](../.github/workflows/release-windows.yml) submits artifacts to SignPath when these secrets are set.

## Option B — Azure Artifact Signing (~$10/month)

1. Create an [Azure Artifact Signing](https://learn.microsoft.com/en-us/azure/artifact-signing/overview) account
2. Add GitHub secrets (see [`.github/workflows/release-windows.yml`](../.github/workflows/release-windows.yml)):
   - `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID`
   - `AZURE_CODESIGNING_ENDPOINT`, `AZURE_CODESIGNING_ACCOUNT`, `AZURE_CODESIGNING_PROFILE`
3. Run workflow **Release Windows** on a version tag before publishing the zip

## Option C — Your own certificate

```powershell
# After build + package, with a .pfx on disk:
$env:SF4E_SIGN_PFX = "C:\path\codesign.pfx"
$env:SF4E_SIGN_PFX_PASSWORD = "..."
powershell -File scripts/sign-release-binaries.ps1 -InputDir msvc-out\relwithdebinfo
```

## Verify signatures after signing

```powershell
Get-AuthenticodeSignature Launcher.exe, Sidecar.dll, RelayHost.exe, Updater.exe | Format-List
```

Status should be **Valid** with publisher **SignPath Foundation** (or your org).

## Microsoft false-positive submission (every unsigned release)

Submit **only these files** (not the whole zip):

- `Launcher.exe`
- `Sidecar.dll`
- `RelayHost.exe`

```powershell
powershell -File scripts/prepare-defender-submission.ps1
```

Upload the output folder at [Microsoft file submission](https://www.microsoft.com/en-us/wdsi/filesubmission) → **Incorrectly detected as malware** → detection `Program:Win32/Wacapew.A!ml`.

## Release policy

- **Latest** on GitHub points at **v0.3.1** until **v0.3.4+** is Authenticode-signed.
- Do not promote unsigned builds to Latest even if netplay fixes are newer.
- Defender false positives (unchanged hook since v0.3.1): [`docs/WINDOWS_DEFENDER.md`](WINDOWS_DEFENDER.md)
- SignPath checklist: [`docs/SIGNPATH_APPLY.md`](SIGNPATH_APPLY.md)
