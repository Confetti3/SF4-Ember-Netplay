# Package Steam P2P experiment with Qt Widgets UI (single Launcher.exe, no Electron/WebView).

param(

    [string]$BuildDir = "",

    [string]$OutDir = "",

    [switch]$SkipNativeBuild

)



$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path $PSScriptRoot -Parent

if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "msvc-build\steam-p2p" }

if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "dist" }



$NativeTargets = @("Launcher", "LauncherApp", "Sidecar", "Updater", "SteamP2PPayloadTest", "SteamP2PProbe")

$RuntimeDlls = @(

    "spdlog.dll", "fmt.dll", "GameNetworkingSockets.dll",

    "GGPO.dll", "libcrypto-3.dll", "libprotobuf.dll", "abseil_dll.dll", "steam_api.dll"

)

$QtDlls = @("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll")

$QtDeps = @("icudt78.dll", "icuin78.dll", "icuuc78.dll", "double-conversion.dll", "pcre2-16.dll", "md4c.dll", "zlib1.dll")

$PackageDllAllowlist = $RuntimeDlls + $QtDlls + $QtDeps + @("Sidecar.dll")

$PackageDllExclude = @("WebView2Loader.dll", "d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll")

$AllowedQtPlugins = @(
    "platforms\qwindows.dll",
    "styles\qmodernwindowsstyle.dll",
    "imageformats\qgif.dll",
    "imageformats\qico.dll",
    "imageformats\qjpeg.dll",
    "networkinformation\qnetworklistmanager.dll",
    "tls\qcertonlybackend.dll",
    "tls\qschannelbackend.dll",
    "generic\qtuiotouchplugin.dll"
)



$Stamp = Get-Date -Format "yyyyMMdd-HHmm"

$PackageName = "sf4e-steam-p2p-$Stamp"

$PackageDir = Join-Path $OutDir $PackageName



function Invoke-VcBuild {

    $cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

    $sdkDir = Join-Path $RepoRoot "dist\steamworks_sdk_164\sdk"

    if (-not (Test-Path $sdkDir)) {

        throw "Steamworks SDK not found at $sdkDir. Extract steamworks_sdk_164.zip to dist/steamworks_sdk_164/sdk"

    }

    $vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"

    $cfgCmd = "cmake -B `"$BuildDir`" -DSF4E_ENABLE_STEAMWORKS_EXPERIMENT=ON -DSF4E_LAUNCHER_QT_UI=ON -DSF4E_STEAMWORKS_SDK_DIR=`"$($sdkDir -replace '\\','/')`""

    $buildCmd = "cmake --build `"$BuildDir`" --target $($NativeTargets -join ' ') -j 8"

    cmd /c "`"$vcvars`" && if not exist `"$BuildDir`\CMakeCache.txt`" ($cfgCmd) else (cmake -B `"$BuildDir`" -DSF4E_ENABLE_STEAMWORKS_EXPERIMENT=ON -DSF4E_LAUNCHER_QT_UI=ON -DSF4E_STEAMWORKS_SDK_DIR=`"$($sdkDir -replace '\\','/')`") && $buildCmd"

    if ($LASTEXITCODE -ne 0) { throw "Native build failed" }

}



function Invoke-WindeployQt {

    param([string]$LauncherExe)

    $windeploy = Join-Path $BuildDir "vcpkg_installed\x64-windows\tools\Qt6\bin\windeployqt.exe"

    if (Test-Path $windeploy) {

        & $windeploy --no-translations --no-compiler-runtime $LauncherExe

        if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

    } else {

        Write-Host "[WARN] windeployqt not found; relying on build output Qt DLLs"

    }

    & (Join-Path $RepoRoot "scripts\sync-x86-qt-plugins.ps1") -Dir (Split-Path $LauncherExe -Parent) -BuildDir $BuildDir

    foreach ($stale in @("platforms", "generic", "imageformats", "networkinformation", "styles", "tls")) {

        $p = Join-Path (Split-Path $LauncherExe -Parent) $stale

        if (Test-Path $p) { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }

    }

}



function Copy-AllowedDlls {

    param(

        [string]$FromDir,

        [string]$ToDir

    )

    foreach ($name in $PackageDllAllowlist) {

        $src = Join-Path $FromDir $name

        if (Test-Path $src) { Copy-Item $src $ToDir -Force }

    }

    Get-ChildItem $FromDir -Filter "*.dll" -File -ErrorAction SilentlyContinue | ForEach-Object {

        if ($PackageDllExclude -contains $_.Name) { return }

        if ($PackageDllAllowlist -contains $_.Name) { return }

        Write-Host "[WARN] Skipping unlisted DLL (not packaged): $($_.Name)"

    }

}

function Copy-AllowedQtPlugins {
    param(
        [string]$FromDir,
        [string]$ToDir
    )

    if (Test-Path $ToDir) { Remove-Item $ToDir -Recurse -Force }
    New-Item -ItemType Directory -Path $ToDir -Force | Out-Null

    foreach ($rel in $AllowedQtPlugins) {
        $src = Join-Path $FromDir $rel
        if (-not (Test-Path $src)) { continue }

        $dst = Join-Path $ToDir $rel
        New-Item -ItemType Directory -Path (Split-Path $dst -Parent) -Force | Out-Null
        Copy-Item $src $dst -Force
    }

    $qwindows = Join-Path $ToDir "platforms\qwindows.dll"
    if (-not (Test-Path $qwindows)) {
        throw "qwindows.dll missing from Qt plugin source"
    }

    Get-ChildItem $FromDir -Recurse -Filter "*.dll" -File -ErrorAction SilentlyContinue | ForEach-Object {
        $rel = $_.FullName.Substring($FromDir.Length).TrimStart([char[]]@('\', '/'))
        if ($AllowedQtPlugins -contains $rel) { return }
        Write-Host "[WARN] Skipping unlisted Qt plugin (not packaged): $rel"
    }
}



function Write-PackageQtConf {

    param([string]$Dir, [switch]$UseDllDir)

    $libraries = if ($UseDllDir) { "dll" } else { "." }

    @"

[Paths]

Prefix=.

Plugins=plugins

Libraries=$libraries

"@ | Set-Content (Join-Path $Dir "qt.conf") -Encoding Ascii

}



Write-Host "=== SF4e Steam P2P + Qt package ==="

Write-Host "Build dir: $BuildDir"

Write-Host "Output:    $PackageDir"

Write-Host ""



if (-not $SkipNativeBuild) {

    Write-Host "[1/4] Building native targets (Qt UI)..."

    Invoke-VcBuild

} else {

    Write-Host "[1/4] Skipping native build (-SkipNativeBuild)"

}



Write-Host "[2/4] Ensuring Qt runtime beside LauncherApp.exe..."

$launcherAppBuilt = Join-Path $BuildDir "LauncherApp.exe"

if (Test-Path $launcherAppBuilt) {

    Invoke-WindeployQt -LauncherExe $launcherAppBuilt

}



Write-Host "[3/4] Assembling package folder..."

Get-Process -Name "Launcher" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

Start-Sleep -Milliseconds 300

if (Test-Path $PackageDir) { Remove-Item -Recurse -Force $PackageDir }

New-Item -ItemType Directory -Path $PackageDir | Out-Null



$readmeDir = Join-Path $PackageDir "readme"

$toolsDir = Join-Path $PackageDir "tools"

$scriptsDir = Join-Path $PackageDir "scripts"

$dllDir = Join-Path $PackageDir "dll"

New-Item -ItemType Directory -Path $readmeDir, $toolsDir, $scriptsDir, $dllDir -Force | Out-Null



# Root: stub Launcher.exe; real UI in dll\LauncherApp.exe

$launcherStub = Join-Path $BuildDir "Launcher.exe"

if (Test-Path $launcherStub) { Copy-Item $launcherStub $PackageDir -Force }

$updater = Join-Path $BuildDir "Updater.exe"

if (Test-Path $updater) { Copy-Item $updater $PackageDir -Force }

$launcherApp = Join-Path $BuildDir "LauncherApp.exe"

if (Test-Path $launcherApp) { Copy-Item $launcherApp $dllDir -Force }



$buildPlugins = Join-Path $BuildDir "plugins"

if (-not (Test-Path (Join-Path $buildPlugins "platforms\qwindows.dll"))) {

    & (Join-Path $RepoRoot "scripts\sync-x86-qt-plugins.ps1") -Dir $BuildDir -BuildDir $BuildDir

}

# Plugins for package root qt.conf; sync build dir from LauncherApp folder if needed
$appPlugins = Join-Path $BuildDir "plugins"
if (-not (Test-Path (Join-Path $appPlugins "platforms\qwindows.dll")) -and (Test-Path $launcherAppBuilt)) {
    & (Join-Path $RepoRoot "scripts\sync-x86-qt-plugins.ps1") -Dir (Split-Path $launcherAppBuilt -Parent) -BuildDir $BuildDir
    $appPlugins = Join-Path (Split-Path $launcherAppBuilt -Parent) "plugins"
    if (Test-Path $appPlugins) { $buildPlugins = $appPlugins }
}

Copy-AllowedQtPlugins -FromDir $buildPlugins -ToDir (Join-Path $PackageDir "plugins")

Write-PackageQtConf -Dir $PackageDir -UseDllDir
@'
[Paths]
Prefix=..
Plugins=../plugins
Libraries=.
'@ | Set-Content (Join-Path $dllDir "qt.conf") -Encoding Ascii



if (Test-Path (Join-Path $BuildDir "steam_appid.txt")) {

    Copy-Item (Join-Path $BuildDir "steam_appid.txt") $PackageDir

    Copy-Item (Join-Path $BuildDir "steam_appid.txt") $dllDir -Force

} else {

    Set-Content -Path (Join-Path $PackageDir "steam_appid.txt") -Value "45760" -NoNewline

    Set-Content -Path (Join-Path $dllDir "steam_appid.txt") -Value "45760" -NoNewline

}



# dll/: all runtime DLLs (Sidecar, Qt, Steam, netplay deps)

Copy-AllowedDlls -FromDir $BuildDir -ToDir $dllDir



# tools/: diagnostics + tools\dll for standalone runs

$toolsDllDir = Join-Path $toolsDir "dll"

New-Item -ItemType Directory -Path $toolsDllDir -Force | Out-Null

foreach ($name in @("SteamP2PProbe.exe", "SteamP2PPayloadTest.exe")) {

    $src = Join-Path $BuildDir $name

    if (Test-Path $src) { Copy-Item $src $toolsDir -Force }

}

foreach ($name in $RuntimeDlls) {

    $src = Join-Path $BuildDir $name

    if (Test-Path $src) { Copy-Item $src $toolsDllDir -Force }

}

Copy-Item (Join-Path $RepoRoot "scripts\run-package-tests.ps1") (Join-Path $toolsDir "run-tests.ps1") -Force

Copy-Item (Join-Path $RepoRoot "scripts\run-offline-test.ps1") (Join-Path $toolsDir "run-offline-test.ps1") -Force

if (Test-Path (Join-Path $BuildDir "steam_appid.txt")) {

    Copy-Item (Join-Path $BuildDir "steam_appid.txt") $toolsDir -Force

}



# readme/: documentation

Copy-Item (Join-Path $RepoRoot "docs\STEAM_P2P_EXPERIMENT.md") $readmeDir -Force

Copy-Item (Join-Path $RepoRoot "docs\TROUBLESHOOTING.md") $readmeDir -ErrorAction SilentlyContinue



@'

SF4e Steam P2P experiment package (Qt UI)

========================================



START HERE

----------

1. Run preflight.cmd in the package root (calls scripts\preflight.ps1).

2. Optional local smoke test: run tools\run-offline-test.ps1 and confirm the in-game Offline Test overlay appears.

3. Double-click Launcher.exe (native Qt UI — no WebView2 or Electron).

4. Steam must be running on both PCs.



Host: Send invite + listen -> wait P2P connected -> Start game

Join: Accept invite + connect -> Start game



Package layout

--------------

Launcher.exe, Updater.exe, qt.conf, plugins/, steam_appid.txt   (package root)

dll\              Sidecar.dll and all runtime/Qt DLLs

readme\           Documentation and BUILD_INFO.txt

scripts\          preflight.ps1

tools\            SteamP2PProbe.exe, SteamP2PPayloadTest.exe, run-offline-test.ps1, tools\dll\



Built: {0}

'@ -f (Get-Date -Format "yyyy-MM-dd HH:mm") | Set-Content (Join-Path $readmeDir "START_HERE.txt")



@'

SF4e Steam P2P (Qt) — quick start

=================================

1. Run preflight.cmd in this folder.

2. Optional local smoke test: tools\run-offline-test.ps1

3. Double-click Launcher.exe.

4. Full docs: readme\START_HERE.txt

5. Diagnostics: tools\ (see readme for SteamP2PProbe usage)

'@ | Set-Content (Join-Path $PackageDir "START_HERE.txt")



$gitRev = "unknown"

try { $gitRev = (git -C $RepoRoot rev-parse --short HEAD 2>$null) } catch {}

@(

    "package=sf4e-steam-p2p-experiment-qt",

    "built=$Stamp",

    "git=$gitRev",

    "ui=Qt6Widgets",

    "native=Launcher.exe",

    "dll_layout=dll/"

) | Set-Content (Join-Path $readmeDir "BUILD_INFO.txt")



Copy-Item (Join-Path $RepoRoot "scripts\preflight-steam.ps1") (Join-Path $scriptsDir "preflight.ps1") -Force

Copy-Item (Join-Path $RepoRoot "preflight.cmd") $PackageDir -Force



Write-Host "[4/4] Creating zip..."

$ZipPath = "$PackageDir.zip"

if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }

Compress-Archive -Path $PackageDir -DestinationPath $ZipPath -Force



Write-Host ""

Write-Host "Package folder: $PackageDir"

Write-Host "Package zip:    $ZipPath"

Write-Host ""

Write-Host "Running tests..."

& (Join-Path $RepoRoot "scripts\run-package-tests.ps1") -PackageDir $PackageDir -BuildDir $BuildDir

