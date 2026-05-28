# sf4e tester preflight - run from the extracted package folder before Launcher.exe.

# Usage: preflight.cmd



param(

    [string]$PackageDir = ""

)



$ErrorActionPreference = "Continue"

if (-not $PackageDir) {

    $PackageDir = if ($PSScriptRoot) { $PSScriptRoot } else { Get-Location }.Path

}



$RequiredFiles = @(

    "Launcher.exe",

    "RelayHost.exe",

    "Sidecar.dll",

    "WebView2Loader.dll",

    "launcher-ui\index.html",

    "launcher-ui\app.js",

    "launcher-ui\styles.css",

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



# WebView2 Runtime (Edge WebView2, not WebView2Loader.dll)

$webview2Ok = $false

$wv2RegPaths = @(

    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",

    "HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

)

foreach ($regPath in $wv2RegPaths) {

    if (Test-Path $regPath) {

        $pv = (Get-ItemProperty -Path $regPath -ErrorAction SilentlyContinue).pv

        if ($pv) {

            $webview2Ok = $true

            Write-Host "[OK]   WebView2 Runtime (registry pv=$pv)"

            break

        }

    }

}

if (-not $webview2Ok) {

    # Fallback: common install path

    $edgeWebView = "${env:ProgramFiles(x86)}\Microsoft\EdgeWebView\Application"

    if (Test-Path $edgeWebView) {

        $webview2Ok = $true

        Write-Host "[OK]   WebView2 Runtime (EdgeWebView folder found)"

    }

}

if (-not $webview2Ok) {

    Write-Host "[WARN] WebView2 Runtime not detected"

    $warnings += "Install Microsoft Edge WebView2 Runtime: https://go.microsoft.com/fwlink/p/?LinkId=2124703"

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
Write-Host "Note: Windows Defender may flag Sidecar.dll as Wacapew.A!ml (false positive)."
Write-Host "      See docs\WINDOWS_DEFENDER.md — Allow on device, or run defender-add-exclusion.ps1 as Admin."

exit 0


