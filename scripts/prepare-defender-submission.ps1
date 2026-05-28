# Prepare minimal file set for Microsoft WDSI false-positive submission.
# Usage: powershell -File scripts/prepare-defender-submission.ps1

param(
    [string]$InputDir = "msvc-out\relwithdebinfo",
    [string]$OutDir = "dist\defender-submission"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Src = Join-Path $RepoRoot $InputDir
$Dst = Join-Path $RepoRoot $OutDir

if (-not (Test-Path $Src)) {
    Write-Error "Build output not found: $Src"
}

New-Item -ItemType Directory -Force -Path $Dst | Out-Null
$files = @("Launcher.exe", "Sidecar.dll", "RelayHost.exe")
foreach ($name in $files) {
    $from = Join-Path $Src $name
    if (-not (Test-Path $from)) {
        Write-Error "Missing $from — build and install first."
    }
    Copy-Item $from (Join-Path $Dst $name) -Force
}

$readme = @"
Submit ONLY these three files (not the zip) to Microsoft:
https://www.microsoft.com/en-us/wdsi/filesubmission

Category: Incorrectly detected as malware/malicious
Detection name: Program:Win32/Wacapew.A!ml
Product: SF4 Netplay Launcher (unofficial USF4 rollback netplay, open source)
Source: https://github.com/Confetti3/SF4-Netplay-Launcher

SHA256:
"@
foreach ($name in $files) {
    $hash = (Get-FileHash (Join-Path $Dst $name) -Algorithm SHA256).Hash
    $readme += [Environment]::NewLine + ($name + " : " + $hash)
}
$readme | Set-Content (Join-Path $Dst "README-SUBMIT.txt") -Encoding UTF8

Write-Host "Prepared: $Dst"
Write-Host "Open https://www.microsoft.com/en-us/wdsi/filesubmission and upload the three .exe/.dll files."
