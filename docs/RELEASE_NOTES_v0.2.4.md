# SF4 Netplay Launcher v0.2.4

Hotfix for in-app **Install update** download failures.

## What's new

- **Updater download fix:** release zip downloads use GitHub API asset URL, improved WinHTTP streaming, and PowerShell fallback
- **Clearer errors:** failed downloads report HTTP/network detail instead of a generic message
- **Update UI:** download errors no longer leave "Downloading update…" stuck under the error banner

## Upgrade from v0.2.3 or earlier

If **Install update** failed before, download this zip manually once, then future in-app updates should work:

1. Download the **team zip** asset below
2. Extract over your existing install folder (or extract to a new folder)
3. Run `preflight.ps1`, then `Launcher.exe`

After v0.2.4, use **Check for updates** / **Install update** on the home screen.

## Prerequisites

- **Ultra Street Fighter IV** on Steam (app 45760)
- [Microsoft Edge WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
- [VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe)

## Install

1. Download the **team zip** asset below (not "Source code" only).
2. Extract the **entire** zip to one folder.
3. Run `preflight.cmd`
4. Run `Launcher.exe` — **Host**, **Join**, or **Offline**

## Support

Include the **Git** line from `BUILD_INFO.txt` when reporting issues.
