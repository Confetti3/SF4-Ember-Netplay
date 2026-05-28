# Windows Defender and antivirus (false positives)

## Is this malware?

**No.** `Program:Win32/Wacapew.A!ml` (and similar alerts on `Sidecar.dll` or `Launcher.exe`) are **heuristic false positives**, not confirmed malware.

The **`!ml`** suffix means Microsoft Defender used **machine learning**, not a specific known virus signature. New, **unsigned** tools that hook into games are often flagged until reputation builds or the publisher submits the files for analysis.

## Why Defender flags this project

| Behavior | Why we do it | Why AV cares |
|----------|--------------|--------------|
| **`Sidecar.dll` injected into USF4** | Rollback netplay (GGPO) requires hooking the game process | Same pattern as cheats/trainers |
| **Microsoft Detours** | Official library for safe in-process hooks | Hooking = suspicious to heuristics |
| **Networking** (`RelayHost.exe`, UDP/TCP relays) | Online play between two PCs | P2P/network tools get extra scrutiny |
| **No Authenticode signature** | Indie / open-source release | Unknown publisher = lower trust score |

Source code is public: [github.com/Confetti3/SF4-Netplay-Launcher](https://github.com/Confetti3/SF4-Netplay-Launcher). You can build the same binaries yourself (see README).

## What to do (installers)

1. Download **only** from the official GitHub release (not random mirrors or “repacked” zips).
2. Check **`BUILD_INFO.txt`** inside the zip — note the **Git** commit and compare to the release page.
3. When Defender shows the alert:
   - Open **See details** and confirm the file is under your extracted folder (e.g. `Sidecar.dll` or `Launcher.exe`).
   - Choose **Allow on device** (or restore from quarantine and add an exclusion).
4. Add a **folder exclusion** (optional, clearer for updates):
   - Windows Security → Virus & threat protection → Manage settings → Exclusions → Add an exclusion → **Folder** → your full install folder (e.g. `C:\Games\SF4-Netplay-Launcher`).

**Do not** disable Defender entirely. Excluding only this folder is enough.

## Verify files (optional)

From PowerShell in the install folder:

```powershell
Get-FileHash Launcher.exe, Sidecar.dll, RelayHost.exe -Algorithm SHA256 | Format-Table
```

Compare hashes with values posted on the GitHub release (when provided) or with a friend on the **same** zip.

## Maintainer actions (reducing false positives over time)

- Submit release zips to [Microsoft malware analysis](https://www.microsoft.com/en-us/wdsi/filesubmission) as **false positive / developer software**.
- Upload the same zip to [VirusTotal](https://www.virustotal.com/) and link results in release notes.
- Long-term: **Authenticode-sign** `Launcher.exe`, `Sidecar.dll`, `RelayHost.exe`, and `Updater.exe` with a stable publisher certificate.

## Still unsure?

- Read `SECURITY.md` in the zip.
- Do not run builds from strangers; use the official release or build from source.
- Report suspicious behavior (unexpected network hosts, keylogging, etc.) privately per `SECURITY.md` — that is different from a generic `Wacapew.A!ml` heuristic hit on first install.
