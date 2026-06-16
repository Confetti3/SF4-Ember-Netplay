# sf4e tester preflight - run from the extracted package folder before Launcher.exe.

# Usage: preflight.cmd (from the package folder; cmd passes -PackageDir automatically)



param(

    [string]$PackageDir = ""

)



$ErrorActionPreference = "Continue"

if (-not $PackageDir) {
    if ($PSScriptRoot) {
        $PackageDir = $PSScriptRoot
    } else {
        $PackageDir = (Get-Location).Path
    }
}



$RequiredFiles = @(

    "Launcher.exe",

    "RelayHost.exe",

    "Sidecar.dll",

    "Updater.exe",

    "qt.conf",

    "plugins\platforms\qwindows.dll",

    "Qt6Core.dll",

    "Qt6Gui.dll",

    "Qt6Widgets.dll",

    "spdlog.dll",

    "fmt.dll",

    "GameNetworkingSockets.dll",

    "GGPO.dll",

    "libcrypto-3.dll",

    "libprotobuf.dll",

    "abseil_dll.dll"

)



$failures = @()

$warnings = @()



Write-Host "SF4 Netplay Launcher preflight"

Write-Host "Package folder: $PackageDir"

Write-Host ""



foreach ($rel in $RequiredFiles) {

    $path = Join-Path $PackageDir $rel

    if (Test-Path $path) {

        Write-Host "[OK]   $rel"

    } else {

        Write-Host "[FAIL] $rel"

        $failures += "Missing file: $rel"

    }

}



# VC++ 2015-2022 x86 (best-effort registry check)

$vcOk = $false

$vcReg = @(

    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x86",

    "HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x86"

)

foreach ($regPath in $vcReg) {

    if (Test-Path $regPath) {

        $installed = (Get-ItemProperty -Path $regPath -ErrorAction SilentlyContinue).Installed

        if ($installed -eq 1) {

            $vcOk = $true

            Write-Host "[OK]   VC++ 2015-2022 Redistributable (x86)"

            break

        }

    }

}

if (-not $vcOk) {

    Write-Host "[WARN] VC++ x86 redistributable not detected"

    $warnings += "Install VC++ x86 redist: https://aka.ms/vs/17/release/vc_redist.x86.exe"

}



$buildInfo = Join-Path $PackageDir "BUILD_INFO.txt"

if (Test-Path $buildInfo) {

    Write-Host ""

    Write-Host "--- BUILD_INFO.txt ---"

    Get-Content $buildInfo | ForEach-Object { Write-Host $_ }

}



Write-Host ""

if ($failures.Count -gt 0) {

    Write-Host "RESULT: FAIL"

    Write-Host "Fix:"

    foreach ($f in $failures) { Write-Host "  - $f" }

    Write-Host "  Re-extract the full zip. Do not copy only Launcher.exe."

    exit 1

}



if ($warnings.Count -gt 0) {

    Write-Host "RESULT: PASS with warnings"

    foreach ($w in $warnings) { Write-Host "  - $w" }

    exit 0

}



Write-Host "RESULT: PASS - run Launcher.exe from this folder."
Write-Host ""
Write-Host "Note: Windows Defender may flag Sidecar.dll as Wacapew.A!ml (false positive on unsigned hook)."
Write-Host "      See docs\WINDOWS_DEFENDER.md - verify release hashes; signed builds are the fix."

exit 0
