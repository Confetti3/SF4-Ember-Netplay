# Code signing (fix Windows Defender false positives)

`Program:Win32/Wacapew.A!ml` on **`Sidecar.dll`** is a **heuristic** flag. Unsigned game-hook DLLs are routinely misclassified. **Authenticode signing** is the reliable fix.

## Option A — SignPath Foundation (free for OSS, recommended)

1. Apply: [signpath.org/apply](https://signpath.org/apply)
2. Repo policy: [`.signpath/signpath.json`](../.signpath/signpath.json)
3. Requirements: MIT license, public GitHub, active maintenance, signing policy in repo
4. After approval: connect SignPath.io to GitHub Actions; each release is signed as **SignPath Foundation**

## Option B — Azure Artifact Signing (~$10/month)

1. Create an [Azure Artifact Signing](https://learn.microsoft.com/en-us/azure/artifact-signing/overview) account
2. Add GitHub secrets (see [`.github/workflows/release-windows.yml`](../.github/workflows/release-windows.yml)):
   - `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID`
   - `AZURE_CODESIGNING_ENDPOINT`, `AZURE_CODESIGNING_ACCOUNT`, `AZURE_CODESIGNING_PROFILE`
3. Run workflow **Release Windows (build + sign)** before publishing the zip

## Option C — Your own certificate

```powershell
# After build + package, with a .pfx on disk:
$env:SF4E_SIGN_PFX = "C:\path\codesign.pfx"
$env:SF4E_SIGN_PFX_PASSWORD = "..."
powershell -File scripts/sign-release-binaries.ps1 -InputDir msvc-out\relwithdebinfo
```

## Microsoft false-positive submission (do every release until signed)

Until signatures are live, submit **only these files** (not the whole zip):

- `Launcher.exe`
- `Sidecar.dll`
- `RelayHost.exe`

```powershell
powershell -File scripts/prepare-defender-submission.ps1
```

Then upload the folder at [Microsoft file submission](https://www.microsoft.com/en-us/wdsi/filesubmission) → **Incorrectly detected as malware** → detection `Program:Win32/Wacapew.A!ml`.

## Friend install workaround (unsigned builds)

```powershell
# Run PowerShell as Administrator, set your install folder:
powershell -File scripts\defender-add-install-exclusion.ps1 -InstallDir "C:\Games\SF4-Netplay-Launcher"
```

This adds a Defender **folder exclusion** only for that path (safer than disabling Defender).
