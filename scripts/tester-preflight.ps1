# Checks an extracted Ember package: every required file is present and
# matches MANIFEST.txt, and no stray program file sits in the folder. It
# changes nothing.
# -Strict (used when packaging) rejects every file outside the package inventory.
# -Interactive (used by preflight.cmd, which players double-click) says what the
# check is for and reports a failure in words instead of a PowerShell error.
param([string]$PackageDir = $PSScriptRoot, [switch]$Strict, [switch]$Interactive)
$ErrorActionPreference = 'Stop'
# Scripts calling this (packaging, Install-Upgrade.ps1) get the error itself.
trap {
    if (!$Interactive) { break }
    Write-Host "FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'Extract the downloaded ZIP again into a new, empty folder, then run this check again.'
    exit 1
}
if ($Interactive) {
    Write-Host 'SF4 Ember Netplay file check (preflight)'
    Write-Host ''
    Write-Host 'This optional check confirms that every Ember file was extracted completely and'
    Write-Host 'unchanged. It changes nothing. It does not install, update or start Ember.'
    Write-Host 'To play, run Launcher.exe. To update, choose Check for updates in Help & about,'
    Write-Host 'or double-click Updater.exe.'
    Write-Host ''
}
$package = (Resolve-Path -LiteralPath $PackageDir).Path.TrimEnd('\','/')
$inventory = Join-Path $package 'PackageInventory.inc'
$allowed = @(); $obsolete = @()
foreach ($line in Get-Content -LiteralPath $inventory) {
    if ($line -match '^SF4E_PACKAGE_(REQUIRED|OPTIONAL|OBSOLETE)\("(.*)"\)') {
        $kind = $Matches[1]; $path = $Matches[2].Replace('\\','\')
        if ($kind -eq 'OBSOLETE') { $obsolete += $path; continue }
        $allowed += $path
        if ($kind -eq 'REQUIRED' -and !(Test-Path -LiteralPath (Join-Path $package $path) -PathType Leaf)) { throw "Missing $path" }
    }
}
foreach ($path in $obsolete) { if (Test-Path -LiteralPath (Join-Path $package $path)) { throw "Obsolete product file: $path" } }
# A stray program file can be loaded by the game or launcher (an old DLL left
# by extracting over a previous version), so it fails the check. Anything else
# a player keeps in the folder, such as the release's .zip.sha256 checksum, is
# harmless: it is reported and skipped.
$programExtensions = @('.dll', '.exe', '.asi', '.sys', '.ocx', '.cpl', '.drv')
$ignored = @{}
foreach ($file in Get-ChildItem -LiteralPath $package -File -Recurse) {
    $relative = $file.FullName.Substring($package.Length + 1)
    if ($relative -in $allowed -or $relative -match '^assets\\selection\\') { continue }
    # The in-game updater's own backups and journal are never loaded.
    if (!$Strict -and $relative -match '^\.ember-update') { $ignored[$relative] = $true; continue }
    if ($Strict) { throw "Unexpected package file: $relative" }
    if ($file.Extension.ToLowerInvariant() -in $programExtensions) { throw "Unexpected program file: $relative. Remove it, or extract the package into an empty folder." }
    Write-Warning "Not part of the package, ignored: $relative"
    $ignored[$relative] = $true
}
$manifestPaths = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $package 'MANIFEST.txt')) {
    if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') { throw 'Malformed manifest' }
    $expected = $Matches[1]; $relative = $Matches[2]
    if ($relative.Contains('..') -or [IO.Path]::IsPathRooted($relative)) { throw 'Invalid manifest path' }
    if ($manifestPaths.ContainsKey($relative)) { throw "Duplicate manifest entry: $relative" }
    $manifestPaths[$relative] = $true
    if ((Get-FileHash -LiteralPath (Join-Path $package $relative) -Algorithm SHA256).Hash -ne $expected) { throw "Hash mismatch: $relative" }
}
foreach ($file in Get-ChildItem -LiteralPath $package -File -Recurse) {
    $relative = $file.FullName.Substring($package.Length + 1)
    if ($relative -ne 'MANIFEST.txt' -and !$ignored.ContainsKey($relative) -and !$manifestPaths.ContainsKey($relative)) { throw "Missing manifest hash: $relative" }
}
$artCount = @(Get-ChildItem -LiteralPath (Join-Path $package 'assets/selection') -Recurse -File -Filter '*.png').Count
if ($artCount -lt 44) { throw 'Fighter artwork is incomplete' }
if ($Interactive) { Write-Host "PASSED: every Ember file is present and unchanged ($artCount artwork images). You can close this window and run Launcher.exe." -ForegroundColor Green }
else { Write-Host "SF4 Ember Netplay preflight passed: required files, obsolete runtime exclusion, manifest hashes, $artCount artwork images." }
