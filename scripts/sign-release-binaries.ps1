# Authenticode-sign shipped binaries (requires SF4E_SIGN_PFX + SF4E_SIGN_PFX_PASSWORD).
# Usage: powershell -File scripts/sign-release-binaries.ps1 -InputDir <folder holding Launcher.exe and the others>
# Signing rewrites the files. The staged binaries under build/current/stage are hashed in the build receipt that
# packaging checks, so sign a copy of the binaries, never the staged files themselves.

param(
    [Parameter(Mandatory = $true)][string]$InputDir,
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Dir = if ([IO.Path]::IsPathRooted($InputDir)) { $InputDir } else { Join-Path $RepoRoot $InputDir }

$pfx = $env:SF4E_SIGN_PFX
$pfxPassword = $env:SF4E_SIGN_PFX_PASSWORD
if (-not $pfx -or -not (Test-Path $pfx)) {
    Write-Error "Set SF4E_SIGN_PFX to your .pfx path."
}
if (-not $pfxPassword) {
    Write-Error "Set SF4E_SIGN_PFX_PASSWORD."
}

# The newest signtool.exe of any installed Windows SDK, x86 tools first.
$kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
$signtool = foreach ($architecture in "x86", "x64") {
    Get-ChildItem -Path (Join-Path $kits "10.*\$architecture\signtool.exe") -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Directory.Parent.Name } -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
$signtool = $signtool | Select-Object -First 1
if (-not $signtool) {
    Write-Error "signtool.exe not found. Install Windows SDK."
}

$files = @("Launcher.exe", "Sidecar.dll", "sf4-net.exe", "Updater.exe")
foreach ($name in $files) {
    $path = Join-Path $Dir $name
    if (-not (Test-Path $path)) {
        Write-Warning "Skip missing $name"
        continue
    }
    Write-Host "Signing $name..."
    & $signtool sign /fd SHA256 /tr $TimestampUrl /td SHA256 /f $pfx /p $pfxPassword $path
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $name" }
    & $signtool verify /pa $path
    if ($LASTEXITCODE -ne 0) { throw "verify failed for $name" }
}
Write-Host "All binaries signed."
