param(
    [string]$PackageDir = "",
    [switch]$Wait
)

$ErrorActionPreference = "Stop"

if (-not $PackageDir) {
    $candidate = Split-Path $PSScriptRoot -Parent
    if (Test-Path (Join-Path $candidate "Launcher.exe")) {
        $PackageDir = $candidate
    } else {
        $PackageDir = Split-Path $candidate -Parent
    }
}

$launcher = Join-Path $PackageDir "Launcher.exe"
if (-not (Test-Path $launcher)) {
    throw "Launcher.exe not found in $PackageDir"
}

Write-Host "Starting offline tester launch..."
Write-Host "Launcher: $launcher"
Write-Host "Logs:     $env:APPDATA\sf4e\logs\launcher.log, sf4e.log, and sidecar_bootstrap.log"

$args = @("--offline", "--dev-overlay")
$process = Start-Process -FilePath $launcher -ArgumentList $args -WorkingDirectory $PackageDir -PassThru
Write-Host "Started Launcher.exe pid $($process.Id)."

if ($Wait) {
    $process.WaitForExit()
    exit $process.ExitCode
}
