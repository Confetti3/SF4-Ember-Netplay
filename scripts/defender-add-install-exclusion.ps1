# Add Windows Defender folder exclusion for the SF4 install directory (Administrator required).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\defender-add-install-exclusion.ps1 -InstallDir "C:\Games\SF4-Netplay-Launcher"

param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDir
)

$ErrorActionPreference = "Stop"
$resolved = (Resolve-Path $InstallDir).Path

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)
if (-not $isAdmin) {
    Write-Error "Run PowerShell as Administrator."
}

Add-MpPreference -ExclusionPath $resolved
Write-Host "Defender exclusion added for: $resolved"
Write-Host "Re-extract the release zip into this folder, then run Launcher.exe."
