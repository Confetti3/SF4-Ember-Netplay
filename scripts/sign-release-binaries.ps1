# Authenticode-sign shipped binaries (requires SF4E_SIGN_PFX + SF4E_SIGN_PFX_PASSWORD).
# Usage: powershell -File scripts/sign-release-binaries.ps1 -InputDir msvc-out\relwithdebinfo

param(
    [string]$InputDir = "msvc-out\relwithdebinfo",
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Dir = Join-Path $RepoRoot $InputDir

$pfx = $env:SF4E_SIGN_PFX
$pwd = $env:SF4E_SIGN_PFX_PASSWORD
if (-not $pfx -or -not (Test-Path $pfx)) {
    Write-Error "Set SF4E_SIGN_PFX to your .pfx path."
}
if (-not $pwd) {
    Write-Error "Set SF4E_SIGN_PFX_PASSWORD."
}

$signtool = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.22621.0\x86\signtool.exe",
    "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x86\signtool.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $signtool) {
    Write-Error "signtool.exe not found. Install Windows SDK."
}

$files = @("Launcher.exe", "Sidecar.dll", "RelayHost.exe", "Updater.exe")
foreach ($name in $files) {
    $path = Join-Path $Dir $name
    if (-not (Test-Path $path)) {
        Write-Warning "Skip missing $name"
        continue
    }
    Write-Host "Signing $name..."
    & $signtool sign /fd SHA256 /tr $TimestampUrl /td SHA256 /f $pfx /p $pwd $path
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $name" }
    & $signtool verify /pa $path
    if ($LASTEXITCODE -ne 0) { throw "verify failed for $name" }
}
Write-Host "All binaries signed."
