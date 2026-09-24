# -Strict (used when packaging) rejects every file outside the package inventory.
param([string]$PackageDir = $PSScriptRoot, [switch]$Strict)
$ErrorActionPreference = 'Stop'
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
Write-Host "SF4 Ember Netplay preflight passed: required files, obsolete runtime exclusion, manifest hashes, $artCount artwork images."
