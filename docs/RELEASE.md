# Publishing SF4 Netplay Launcher releases

> **Experimental unofficial port** — release notes and zip assets should describe this as **not production-ready** friends-only test software. See [docs/SCOPE_AND_LIMITATIONS.md](SCOPE_AND_LIMITATIONS.md) and [docs/RELEASE_NOTES_TEMPLATE.md](RELEASE_NOTES_TEMPLATE.md).

## One-command release (recommended)

From the repository root, with Visual Studio build tools and `gh` CLI installed:

```powershell
powershell -NoProfile -File scripts/github-release.ps1 -Tag v0.2.0
```

This builds, packages, validates the manifest, and creates a GitHub Release with the zip attached.

If PowerShell blocks script execution on your dev PC, run once:

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

## Manual steps

1. **Build and package**

   ```powershell
   powershell -NoProfile -File scripts/release-team-build.ps1 -VersionLabel v0.2.0
   ```

2. **Create GitHub Release**

   ```powershell
   gh release create v0.2.0 dist/sf4-netplay-launcher-*.zip --title "SF4 Netplay Launcher v0.2.0" --notes-file docs/RELEASE_NOTES_TEMPLATE.md
   ```

3. **Share with testers**

   - Link: `https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest`
   - Tell them this is **experimental** test software — not production-ready; sessions may fail
   - Tell them to download the **Assets** zip (not source-only)
   - Same zip on both PCs; run `preflight.cmd` then `Launcher.exe`
   - Link [docs/TROUBLESHOOTING.md](TROUBLESHOOTING.md) in release notes (see template) so GitHub Releases point players at the troubleshooting guide

## What ships in the zip

- `Launcher.exe`, `Sidecar.dll`, **`RelayHost.exe`**, `Updater.exe`, `WebView2Loader.dll`, `launcher-ui/`
- Runtime DLLs (GNS, GGPO, spdlog, etc.)
- `START_HERE.md`, `preflight.ps1`, `MANIFEST.txt`, `BUILD_INFO.txt`, `ATTRIBUTION.md`
- `docs/TROUBLESHOOTING.md` (player troubleshooting — also linked from each release on GitHub)

## Version tags

Use semantic tags like `v0.2.0`. Match `-VersionLabel` in package scripts for support threads.
