# Preflight for Steam P2P experiment package (Qt UI).

param([string]$PackageDir = "")



$ErrorActionPreference = "Continue"

if (-not $PackageDir) {

    $PackageDir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

}

# When invoked as scripts\preflight.ps1, check parent package root.

$root = $PackageDir

if ((Split-Path $PackageDir -Leaf) -eq "scripts") {

    $root = Split-Path $PackageDir -Parent

}



$Required = @(

    "Launcher.exe",

    "Updater.exe",

    "steam_appid.txt",

    "dll\LauncherApp.exe",

    "dll\Sidecar.dll",

    "dll\steam_api.dll",

    "dll\steam_appid.txt",

    "dll\spdlog.dll",

    "dll\fmt.dll",

    "dll\GameNetworkingSockets.dll",

    "dll\GGPO.dll",

    "dll\libcrypto-3.dll",

    "dll\libprotobuf.dll",

    "dll\abseil_dll.dll"

)



$QtRequired = @(

    "dll\Qt6Core.dll",

    "dll\Qt6Gui.dll",

    "dll\Qt6Widgets.dll",

    "qt.conf",

    "plugins\platforms\qwindows.dll"

)



$Optional = @(

    "START_HERE.txt",

    "plugins",

    "scripts\preflight.ps1",

    "readme\STEAM_P2P_EXPERIMENT.md",

    "readme\START_HERE.txt",

    "readme\BUILD_INFO.txt",

    "tools\SteamP2PProbe.exe",

    "tools\SteamP2PPayloadTest.exe",

    "tools\run-offline-test.ps1",

    "tools\dll\steam_api.dll"

)



$failures = @()

$warnings = @()



Write-Host "SF4e Steam P2P package preflight (Qt)"

Write-Host "Folder: $root"

Write-Host ""



foreach ($rel in $Required) {

    $path = Join-Path $root $rel

    if (Test-Path $path) {

        Write-Host "[OK]   $rel"

    } else {

        Write-Host "[FAIL] $rel"

        $failures += $rel

    }

}



foreach ($rel in $QtRequired) {

    $path = Join-Path $root $rel

    if (Test-Path $path) {

        Write-Host "[OK]   $rel"

    } else {

        Write-Host "[FAIL] $rel"

        $failures += $rel

    }

}



foreach ($rel in $Optional) {

    $path = Join-Path $root $rel

    if (Test-Path $path) {

        Write-Host "[OK]   $rel (optional)"

    } else {

        Write-Host "[WARN] $rel (optional)"

        $warnings += $rel

    }

}



# Legacy / flat-layout / windeployqt cruft should not appear

$legacy = @(

    "SF4NetplayLauncher.exe",

    "launcher-ui\index.html",

    "WebView2Loader.dll",

    "Sidecar.dll",

    "Qt6Widgets.dll",

    "preflight.ps1",

    "BUILD_INFO.txt",

    "d3dcompiler_47.dll",

    "dxcompiler.dll",

    "dxil.dll"

)

foreach ($rel in $legacy) {

    $path = Join-Path $root $rel

    if (Test-Path $path) {

        Write-Host "[WARN] Legacy or flat-layout file at package root (use dll\): $rel"

        $warnings += $rel

    }

}



if (-not (Get-Process -Name "steam" -ErrorAction SilentlyContinue)) {

    Write-Host "[WARN] Steam client is not running"

    $warnings += "Start Steam before using netplay"

} else {

    Write-Host "[OK]   Steam client process detected"

}



Write-Host ""

if ($failures.Count -eq 0) {

    Write-Host "Preflight PASSED ($($warnings.Count) warning(s))"

    exit 0

}

Write-Host "Preflight FAILED: missing $($failures.Count) required file(s)"

exit 1

