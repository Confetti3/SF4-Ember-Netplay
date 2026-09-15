# Build and validate a small hash-addressed upgrade between two complete packages.
param([Parameter(Mandatory=$true)][string]$FromVersion,
      [Parameter(Mandatory=$true)][string]$ToVersion,
      [Parameter(Mandatory=$true)][string]$BaseZip,
      [Parameter(Mandatory=$true)][string]$TargetZip,
      [string]$OutDir = 'dist')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
if ($FromVersion -notmatch '^\d+\.\d+\.\d+$' -or $ToVersion -notmatch '^\d+\.\d+\.\d+$' -or $FromVersion -eq $ToVersion) {
    throw 'Use distinct semantic versions such as 0.8.2 and 0.8.3.'
}

function Assert-ZipSidecar([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (!(Test-Path -LiteralPath $full -PathType Leaf) -or !(Test-Path -LiteralPath ($full + '.sha256') -PathType Leaf)) {
        throw "Package or SHA-256 sidecar missing: $full"
    }
    $line = (Get-Content -LiteralPath ($full + '.sha256') -Raw).Trim()
    if ($line -notmatch '^([0-9a-fA-F]{64})  ([^\\/]+\.zip)$' -or $Matches[2] -cne [IO.Path]::GetFileName($full)) {
        throw "Malformed package checksum sidecar: $full.sha256"
    }
    $actual = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
    if ($actual -ine $Matches[1]) { throw "Package checksum mismatch: $full" }
    return $actual.ToLowerInvariant()
}

function PackageRoot([string]$Extracted) {
    $items = @(Get-ChildItem -LiteralPath $Extracted -Force)
    if ($items.Count -eq 1 -and $items[0].PSIsContainer) { return $items[0].FullName }
    return $Extracted
}

function Read-Manifest([string]$Path) {
    $entries = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$' -or $entries.ContainsKey($Matches[2])) { throw "Malformed manifest: $Path" }
        $entries[$Matches[2]] = $Matches[1]
    }
    return $entries
}

function Copy-One([string]$Source, [string]$Destination) {
    $null = New-Item -ItemType Directory -Path (Split-Path $Destination -Parent) -Force
    Copy-Item -LiteralPath $Source -Destination $Destination
}

$baseZipPath = [IO.Path]::GetFullPath($BaseZip)
$targetZipPath = [IO.Path]::GetFullPath($TargetZip)
$basePackageHash = Assert-ZipSidecar $baseZipPath
$targetPackageHash = Assert-ZipSidecar $targetZipPath
$outputRoot = if ([IO.Path]::IsPathRooted($OutDir)) { [IO.Path]::GetFullPath($OutDir) } else { [IO.Path]::GetFullPath((Join-Path $repo $OutDir)) }
$destination = Join-Path $outputRoot "upgrade-sf4-ember-netplay-$FromVersion-to-$ToVersion"
if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath ($destination + '.zip'))) { throw "Upgrade destination already exists: $destination" }
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('sf4-ember-upgrade-build-' + [Guid]::NewGuid().ToString('N'))
$baseExtract = Join-Path $tempRoot 'base'
$targetExtract = Join-Path $tempRoot 'target'
try {
    $null = New-Item -ItemType Directory -Path $baseExtract,$targetExtract,$destination -Force
    Expand-Archive -LiteralPath $baseZipPath -DestinationPath $baseExtract
    Expand-Archive -LiteralPath $targetZipPath -DestinationPath $targetExtract
    $baseRoot = PackageRoot $baseExtract
    $targetRoot = PackageRoot $targetExtract
    $baseManifestPath = Join-Path $baseRoot 'MANIFEST.txt'
    $targetManifestPath = Join-Path $targetRoot 'MANIFEST.txt'
    if (!(Test-Path -LiteralPath $baseManifestPath) -or !(Test-Path -LiteralPath $targetManifestPath)) { throw 'A complete package manifest is missing.' }
    $before = Read-Manifest $baseManifestPath
    $after = Read-Manifest $targetManifestPath
    $baseManifestHash = (Get-FileHash -LiteralPath $baseManifestPath -Algorithm SHA256).Hash
    $targetManifestHash = (Get-FileHash -LiteralPath $targetManifestPath -Algorithm SHA256).Hash
    $before['MANIFEST.txt'] = $baseManifestHash
    $after['MANIFEST.txt'] = $targetManifestHash
    $changed = @($after.Keys | Where-Object { !$before.ContainsKey($_) -or $before[$_] -ine $after[$_] } | Sort-Object)
    $removed = @($before.Keys | Where-Object { !$after.ContainsKey($_) } | Sort-Object)
    if (!$changed.Count) { throw 'The package manifests contain no upgrade work.' }
    $unchanged = @($after.Keys | Where-Object { $before.ContainsKey($_) -and $before[$_] -ieq $after[$_] }).Count

    Copy-Item -LiteralPath $baseManifestPath -Destination (Join-Path $destination 'base-manifest.txt')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'upgrade\Install-Upgrade.ps1') -Destination (Join-Path $destination 'Install-Upgrade.ps1')
    Set-Content -LiteralPath (Join-Path $destination 'Install Upgrade.cmd') -Encoding ASCII -Value '@powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Upgrade.ps1"'
    foreach ($optional in @('ATTRIBUTION.md','LICENSE')) {
        $source = Join-Path $targetRoot $optional
        if (Test-Path -LiteralPath $source -PathType Leaf) { Copy-Item -LiteralPath $source -Destination (Join-Path $destination $optional) }
    }
    $notices = Join-Path $targetRoot 'notices'
    if (Test-Path -LiteralPath $notices -PathType Container) { Copy-Item -LiteralPath $notices -Destination $destination -Recurse }
    $payload = Join-Path $destination 'payload'
    foreach ($relative in $changed) { Copy-One (Join-Path $targetRoot $relative) (Join-Path $payload $relative) }
    $installerHash = (Get-FileHash -LiteralPath (Join-Path $destination 'Install-Upgrade.ps1') -Algorithm SHA256).Hash
    $targetReceipt = Get-Content -LiteralPath (Join-Path $targetRoot 'build-provenance.json') -Raw | ConvertFrom-Json
    [ordered]@{
        schema=1;from=$FromVersion;to=$ToVersion;baseManifestSha256=$baseManifestHash;targetManifestSha256=$targetManifestHash
        basePackageSha256=$basePackageHash;targetPackageSha256=$targetPackageHash;changedFiles=$changed;removedFiles=$removed
        unchangedFiles=$unchanged;payloadSourceRevision=$targetReceipt.baseRevision;installerSha256=$installerHash
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'upgrade.json') -Encoding UTF8
    $guide = @"
# Upgrade SF4 Ember Netplay $FromVersion to $ToVersion

This smaller download upgrades an intact published **Ember $FromVersion** folder to **$ToVersion** without downloading the unchanged artwork and other large files. It replaces $($changed.Count) files, removes $($removed.Count) obsolete files and reuses $unchanged unchanged files. It is not a fresh installation or an upgrade from the legacy launcher.

## Install

1. Close Ultra Street Fighter IV and Ember.
2. Extract this upgrade ZIP into a separate folder outside your Ember installation.
3. Double-click **Install Upgrade.cmd**.
4. Select your existing **Ember $FromVersion folder containing Launcher.exe**.
5. Wait for the verified completion message, then run Launcher.exe from the upgraded folder.

The installer verifies the complete existing package and upgrade payload before changing anything. It constructs and preflights the complete $ToVersion layout locally, creates a backup beside the Ember folder, changes only the declared product files, and verifies the final package. Personal files and preferences under **%APPDATA%\sf4e** are preserved. It does not launch or stop the game.

For a non-modifying check:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Upgrade.ps1 -InstallDir "C:\Games\Ember" -CheckOnly
~~~

If the check reports an interrupted transaction, keep the backup (either the sibling Ember-backup folder or the installation's .ember-update-backups folder) and run recovery from this extracted package:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Upgrade.ps1 -InstallDir "C:\Games\Ember" -RecoverOnly
~~~

Recovery verifies the saved bytes before restoring them. If it reports missing or damaged evidence, preserve the installation, transaction and backup instead of retrying a normal update.

If the selected folder is modified or is not the exact published $FromVersion package, use the complete $ToVersion package instead. Everyone in a room must use the same Ember version.

Release notes: https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v$ToVersion
"@
    Set-Content -LiteralPath (Join-Path $destination 'START_HERE.md') -Encoding UTF8 -Value $guide
    $upgradeInventory = Get-ChildItem -LiteralPath $destination -File -Recurse | Sort-Object FullName | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.FullName.Substring($destination.Length + 1)
    }
    Set-Content -LiteralPath (Join-Path $destination 'UPGRADE_MANIFEST.txt') -Encoding UTF8 -Value $upgradeInventory

    # Exercise both the non-mutating check and a real upgrade against temporary
    # extraction of the exact base package. Extra user files must survive.
    $extra = Join-Path $baseRoot 'user-file-preservation-check.txt'
    Set-Content -LiteralPath $extra -Encoding ASCII -Value 'preserve'
    & (Join-Path $destination 'Install-Upgrade.ps1') -InstallDir $baseRoot -CheckOnly
    & (Join-Path $destination 'Install-Upgrade.ps1') -InstallDir $baseRoot
    if (!(Test-Path -LiteralPath $extra -PathType Leaf)) { throw 'Upgrade removed an unrelated user file.' }
    foreach ($relative in $after.Keys) {
        $path = Join-Path $baseRoot $relative
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path).Hash -ine $after[$relative]) {
            throw "Upgraded validation tree differs from target: $relative"
        }
    }
    foreach ($relative in $removed) { if (Test-Path -LiteralPath (Join-Path $baseRoot $relative)) { throw "Removed file survived validation: $relative" } }

    Compress-Archive -LiteralPath $destination -DestinationPath ($destination + '.zip')
    $hash = (Get-FileHash -LiteralPath ($destination + '.zip') -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath ($destination + '.zip.sha256') -Encoding ASCII -Value "$hash  $([IO.Path]::GetFileName($destination)).zip"
    $script:UpgradeZipPath = $destination + '.zip'
    $script:UpgradeFolderPath = $destination
    Write-Host "SF4 Ember Netplay upgrade package: $script:UpgradeZipPath"
    Write-Host "Upgrade payload: $($changed.Count) changed, $($removed.Count) removed, $unchanged unchanged files reused"
} finally {
    $resolved = [IO.Path]::GetFullPath($tempRoot)
    if ((Split-Path $resolved -Leaf) -like 'sf4-ember-upgrade-build-*' -and $resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    }
}
