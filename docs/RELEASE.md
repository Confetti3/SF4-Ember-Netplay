# Publishing SF4 Enhanced releases

## One-command release (recommended)

From the repository root, with Visual Studio build tools and `gh` CLI installed:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/github-release.ps1 -Tag v0.2.0
```

This builds, packages, validates the manifest, and creates a GitHub Release with the zip attached.

## Manual steps

1. **Build and package**

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts/release-team-build.ps1 -VersionLabel v0.2.0
   ```

2. **Create GitHub Release**

   ```powershell
   gh release create v0.2.0 dist/sf4-enhanced-team-*.zip --title "SF4 Enhanced v0.2.0" --notes-file docs/RELEASE_NOTES_TEMPLATE.md
   ```

3. **Share with testers**

   - Link: `https://github.com/Confetti3/SF4e/releases/latest`
   - Tell them to download the **Assets** zip (not source-only)
   - Same zip on both PCs; run `preflight.ps1` then `Launcher.exe`

## What ships in the zip

- `Launcher.exe`, `Sidecar.dll`, **`RelayHost.exe`**, `WebView2Loader.dll`, `launcher-ui/`
- Runtime DLLs (GNS, GGPO, spdlog, etc.)
- `START_HERE.md`, `preflight.ps1`, `MANIFEST.txt`, `BUILD_INFO.txt`, `ATTRIBUTION.md`

## Version tags

Use semantic tags like `v0.2.0`. Match `-VersionLabel` in package scripts for support threads.
