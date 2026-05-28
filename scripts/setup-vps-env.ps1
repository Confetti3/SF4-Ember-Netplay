# Persist SF4E VPS credentials for deploy scripts (User-level env vars).
# Usage:
#   .\scripts\setup-vps-env.ps1                    # reads scripts\.vps-env.ps1 if present
#   .\scripts\setup-vps-env.ps1 -FromSecretsFile   # same
#   .\scripts\setup-vps-env.ps1 -Password "..."    # avoid saving to file (session only unless -Persist)
param(
    [string]$Password,
    [string]$VpsHost = "",
    [switch]$FromSecretsFile,
    [switch]$Persist = $true
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$secretsFile = Join-Path $PSScriptRoot ".vps-env.ps1"

if (($FromSecretsFile -or -not $Password) -and (Test-Path -LiteralPath $secretsFile)) {
    . $secretsFile
    if (-not $Password -and $env:SF4E_VPS_PASSWORD) {
        $Password = $env:SF4E_VPS_PASSWORD
    }
}

if (-not $Password) {
    $secure = Read-Host "SF4E VPS password (root@74.208.200.95)" -AsSecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        $Password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
    }
}

if (-not $Password) {
    Write-Error "No password provided."
}

if (-not $VpsHost) {
    $VpsHost = [Environment]::GetEnvironmentVariable("SF4E_VPS_HOST", "User")
    if (-not $VpsHost) { $VpsHost = "74.208.200.95" }
}

if ($Persist) {
    [Environment]::SetEnvironmentVariable("SF4E_VPS_PASSWORD", $Password, "User")
    [Environment]::SetEnvironmentVariable("SF4E_VPS_HOST", $VpsHost, "User")
    if (-not [Environment]::GetEnvironmentVariable("SF4E_VPS_USER", "User")) {
        [Environment]::SetEnvironmentVariable("SF4E_VPS_USER", "root", "User")
    }
    Write-Host "Saved SF4E_VPS_PASSWORD and SF4E_VPS_HOST to your Windows User environment."
    Write-Host "Open a new terminal (or restart Cursor) for other apps to see the variables."
}

$env:SF4E_VPS_PASSWORD = $Password
$env:SF4E_VPS_HOST = $VpsHost
if (-not $env:SF4E_VPS_USER) { $env:SF4E_VPS_USER = "root" }

Write-Host "Current session: SF4E_VPS_HOST=$($env:SF4E_VPS_HOST) (password set, not shown)"
