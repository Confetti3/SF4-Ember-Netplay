# Build SF4NetplayLauncher.exe (Electron portable) and optionally merge native binaries.
param(
    [string]$NativeDir = "",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent
$ElectronRoot = Join-Path $RepoRoot "electron-launcher"

if (-not $OutDir) {
    $OutDir = Join-Path $ElectronRoot "dist"
}

Push-Location $ElectronRoot
try {
    if (-not (Test-Path "node_modules\electron-builder")) {
        Write-Host "Installing electron-launcher dependencies..."
        npm install
    }
    npm run pack
    if ($LASTEXITCODE -ne 0) { throw "electron-builder failed" }
}
finally {
    Pop-Location
}

$PortableExe = Get-ChildItem -Path $OutDir -Filter "SF4NetplayLauncher-*.exe" -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $PortableExe) {
    throw "Portable exe not found under $OutDir"
}

Write-Host "Built: $($PortableExe.FullName)"

if ($NativeDir) {
    if (-not (Test-Path $NativeDir)) {
        throw "NativeDir not found: $NativeDir"
    }
    $StageDir = Join-Path $OutDir "sf4e-electron-package"
    if (Test-Path $StageDir) {
        Remove-Item -Recurse -Force $StageDir
    }
    New-Item -ItemType Directory -Path $StageDir | Out-Null

    Copy-Item $PortableExe.FullName (Join-Path $StageDir "SF4NetplayLauncher.exe")
    $NativeFiles = @(
        "Launcher.exe",
        "Sidecar.dll",
        "steam_api.dll",
        "WebView2Loader.dll",
        "Updater.exe",
        "RelayHost.exe"
    )
    foreach ($name in $NativeFiles) {
        $src = Join-Path $NativeDir $name
        if (Test-Path $src) {
            Copy-Item $src $StageDir
        }
    }
    $UiSrc = Join-Path $NativeDir "launcher-ui"
    if (Test-Path $UiSrc) {
        Copy-Item -Recurse $UiSrc (Join-Path $StageDir "launcher-ui")
    }
    Write-Host "Portable package folder: $StageDir"
    Write-Host "Run SF4NetplayLauncher.exe from that folder (native Launcher.exe must stay beside it)."
}
