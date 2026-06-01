# Package Steam P2P experiment + Electron UI for testers (single folder + zip).
param(
    [string]$BuildDir = "",
    [string]$OutDir = "",
    [switch]$SkipNativeBuild,
    [switch]$SkipElectronBuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "msvc-build\steam-p2p" }
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "dist" }

$NativeTargets = @("Launcher", "Sidecar", "SteamP2PPayloadTest", "SteamP2PProbe")
$RuntimeDlls = @(
    "WebView2Loader.dll", "spdlog.dll", "fmt.dll", "GameNetworkingSockets.dll",
    "GGPO.dll", "libcrypto-3.dll", "libprotobuf.dll", "abseil_dll.dll", "steam_api.dll"
)
$CopyExes = @("Launcher.exe", "Sidecar.dll", "SteamP2PProbe.exe", "SteamP2PPayloadTest.exe")
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
    $buildCmd = "cmake --build `"$BuildDir`" --target $($NativeTargets -join ' ') -j 8"
    $cfgCmd = "cmake -B `"$BuildDir`" -DSF4E_ENABLE_STEAMWORKS_EXPERIMENT=ON -DSF4E_STEAMWORKS_SDK_DIR=`"$($sdkDir -replace '\\','/')`""
    cmd /c "`"$vcvars`" && if not exist `"$BuildDir`\CMakeCache.txt`" $cfgCmd && $buildCmd"
    if ($LASTEXITCODE -ne 0) { throw "Native build failed" }
}

Write-Host "=== SF4e Steam P2P + Electron package ==="
Write-Host "Build dir: $BuildDir"
Write-Host "Output:    $PackageDir"
Write-Host ""

if (-not $SkipNativeBuild) {
    Write-Host "[1/5] Building native targets..."
    Invoke-VcBuild
} else {
    Write-Host "[1/5] Skipping native build (-SkipNativeBuild)"
}

Write-Host "[2/5] Syncing launcher-ui to build folder..."
$cmakeExe = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmakeExe -E copy_directory (Join-Path $RepoRoot "launcher-ui") (Join-Path $BuildDir "launcher-ui") | Out-Null

if (-not $SkipElectronBuild) {
    Write-Host "[3/5] Building SF4NetplayLauncher.exe..."
    & (Join-Path $RepoRoot "scripts\build-electron-launcher.ps1") -NativeDir $BuildDir -OutDir (Join-Path $RepoRoot "electron-launcher\dist")
} else {
    Write-Host "[3/5] Skipping Electron build (-SkipElectronBuild)"
}

Write-Host "[4/5] Assembling package folder..."
Get-Process -Name "Launcher","SF4NetplayLauncher" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300
if (Test-Path $PackageDir) { Remove-Item -Recurse -Force $PackageDir }
New-Item -ItemType Directory -Path $PackageDir | Out-Null

foreach ($name in $CopyExes) {
    $src = Join-Path $BuildDir $name
    if (Test-Path $src) { Copy-Item $src $PackageDir -Force }
}
foreach ($name in $RuntimeDlls) {
    $src = Join-Path $BuildDir $name
    if (Test-Path $src) { Copy-Item $src $PackageDir -Force }
}
Copy-Item -Recurse (Join-Path $BuildDir "launcher-ui") (Join-Path $PackageDir "launcher-ui") -Force

$Portable = Get-ChildItem (Join-Path $RepoRoot "electron-launcher\dist") -Filter "SF4NetplayLauncher-*.exe" -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($Portable) {
    Copy-Item $Portable.FullName (Join-Path $PackageDir "SF4NetplayLauncher.exe") -Force
} else {
    throw "SF4NetplayLauncher portable exe not found; run electron build first"
}

if (Test-Path (Join-Path $BuildDir "steam_appid.txt")) {
    Copy-Item (Join-Path $BuildDir "steam_appid.txt") $PackageDir
} else {
    Set-Content -Path (Join-Path $PackageDir "steam_appid.txt") -Value "45760" -NoNewline
}

Copy-Item (Join-Path $RepoRoot "docs\STEAM_P2P_EXPERIMENT.md") $PackageDir -Force
Copy-Item (Join-Path $RepoRoot "docs\TROUBLESHOOTING.md") $PackageDir -ErrorAction SilentlyContinue

@'
SF4e Steam P2P experiment package
=================================

START HERE
----------
1. Run preflight.cmd (or preflight.ps1) in this folder.
2. Double-click SF4NetplayLauncher.exe (recommended UI).
   Or run Launcher.exe for WebView2 UI (needs Edge WebView2 Runtime).
3. Steam must be running on both PCs.

Host: Send invite + listen -> wait Connected -> Start game
Join: Accept invite + connect -> Start game

Files
-----
SF4NetplayLauncher.exe  Electron UI (use this)
Launcher.exe            Native backend (required beside UI exe)
Sidecar.dll, steam_api.dll, runtime DLLs
launcher-ui/            Used by Launcher.exe WebView mode

Built: {0}
'@ -f (Get-Date -Format "yyyy-MM-dd HH:mm") | Set-Content (Join-Path $PackageDir "START_HERE.txt")

Copy-Item (Join-Path $RepoRoot "scripts\preflight-steam.ps1") (Join-Path $PackageDir "preflight.ps1") -Force
Copy-Item (Join-Path $RepoRoot "preflight.cmd") $PackageDir -Force
Copy-Item (Join-Path $RepoRoot "scripts\run-package-tests.ps1") (Join-Path $PackageDir "run-tests.ps1") -Force

$gitRev = "unknown"
try { $gitRev = (git -C $RepoRoot rev-parse --short HEAD 2>$null) } catch {}
@(
    "package=sf4e-steam-p2p-experiment",
    "built=$Stamp",
    "git=$gitRev",
    "electron=SF4NetplayLauncher.exe",
    "native=Launcher.exe --electron-ipc"
) | Set-Content (Join-Path $PackageDir "BUILD_INFO.txt")

Write-Host "[5/5] Creating zip..."
$ZipPath = "$PackageDir.zip"
if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
Compress-Archive -Path $PackageDir -DestinationPath $ZipPath -Force

Write-Host ""
Write-Host "Package folder: $PackageDir"
Write-Host "Package zip:    $ZipPath"
Write-Host ""
Write-Host "Running tests..."
& (Join-Path $RepoRoot "scripts\run-package-tests.ps1") -PackageDir $PackageDir -BuildDir $BuildDir
