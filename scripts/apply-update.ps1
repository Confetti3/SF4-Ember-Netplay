# Applies a staged SF4 Enhanced package over the install directory after the launcher exits.
# Usage: powershell -ExecutionPolicy Bypass -File apply-update.ps1 -InstallDir "C:\Games\SF4e" -StagingDir "$env:TEMP\sf4e-update-v0.2.2\package" -WaitPid 12345

param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDir,
    [Parameter(Mandatory = $true)]
    [string]$StagingDir,
    [int]$WaitPid = 0
)

$ErrorActionPreference = "Stop"
$logPath = Join-Path $env:TEMP "sf4e-update.log"

function Write-Log([string]$Message) {
    $line = "{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    Add-Content -Path $logPath -Value $line
}

try {
    Write-Log "apply-update start InstallDir=$InstallDir StagingDir=$StagingDir WaitPid=$WaitPid"

    if (-not (Test-Path $InstallDir)) {
        throw "Install directory not found: $InstallDir"
    }
    if (-not (Test-Path $StagingDir)) {
        throw "Staging directory not found: $StagingDir"
    }

    if ($WaitPid -gt 0) {
        $deadline = (Get-Date).AddSeconds(30)
        while ((Get-Date) -lt $deadline) {
            $proc = Get-Process -Id $WaitPid -ErrorAction SilentlyContinue
            if (-not $proc) { break }
            Start-Sleep -Milliseconds 200
        }
    }

    Get-Process RelayHost -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500

    $robocopyArgs = @($StagingDir, $InstallDir, "/MIR", "/R:2", "/W:2", "/NFL", "/NDL", "/NJH", "/NJS")
    Write-Log ("robocopy " + ($robocopyArgs -join " "))
    & robocopy @robocopyArgs | Out-Null
    $rc = $LASTEXITCODE
    if ($rc -gt 7) {
        throw "robocopy failed with exit code $rc"
    }

    $launcher = Join-Path $InstallDir "Launcher.exe"
    if (-not (Test-Path $launcher)) {
        throw "Launcher.exe missing after update: $launcher"
    }

    Write-Log "starting Launcher.exe"
    Start-Process -FilePath $launcher -WorkingDirectory $InstallDir
    Write-Log "apply-update complete"
    exit 0
}
catch {
    Write-Log ("ERROR: " + $_.Exception.Message)
    Write-Error $_.Exception.Message
    exit 1
}
