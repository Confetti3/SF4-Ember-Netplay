# Hash-validated SF4 Ember Netplay incremental upgrade installer.
[CmdletBinding()]
param([string]$InstallDir = '', [switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function SafePath([string]$Root, [string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative) -or $Relative -match '[:*?"<>|]' -or
        @($Relative -split '[\\/]' | Where-Object { $_ -in '', '.', '..' -or $_.EndsWith('.') -or $_.EndsWith(' ') }).Count) {
        throw "Invalid package path: $Relative"
    }
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    $path = [IO.Path]::GetFullPath([IO.Path]::Combine($rootPath + '\', $Relative))
    if (!$path.StartsWith($rootPath + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Path escapes the selected folder.' }
    $walk = $path
    while ($walk) {
        if ([IO.File]::Exists($walk) -or [IO.Directory]::Exists($walk)) {
            if ([IO.File]::GetAttributes($walk) -band [IO.FileAttributes]::ReparsePoint) { throw "Linked paths are not supported: $walk" }
        }
        $parentDirectory = [IO.Directory]::GetParent($walk)
        $walk = if ($parentDirectory) { $parentDirectory.FullName } else { $null }
    }
    return $path
}

function ReadManifest([string]$Path, [string]$Root) {
    $entries = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') { throw "Malformed manifest: $Path" }
        $digest = $Matches[1]; $relative = $Matches[2]
        $null = SafePath $Root $relative
        if ($entries.ContainsKey($relative)) { throw "Duplicate manifest entry: $relative" }
        $entries[$relative] = $digest
    }
    return $entries
}

function VerifyFiles([string]$Root, [hashtable]$Entries) {
    foreach ($relative in $Entries.Keys) {
        $path = SafePath $Root $relative
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing file: $relative" }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $Entries[$relative]) {
            throw "File does not match the supported package: $relative"
        }
    }
}

function VerifyAbsent([string]$Root, [string[]]$Entries) {
    foreach ($relative in $Entries) {
        if (Test-Path -LiteralPath (SafePath $Root $relative)) { throw "Obsolete product file is still present: $relative" }
    }
}

function SameList($Left, $Right) {
    $a = @($Left | ForEach-Object { [string]$_ } | Sort-Object)
    $b = @($Right | ForEach-Object { [string]$_ } | Sort-Object)
    if ($a.Count -ne $b.Count) { return $false }
    for ($i = 0; $i -lt $a.Count; ++$i) { if ($a[$i] -cne $b[$i]) { return $false } }
    return $true
}

function CopyOne([string]$Source, [string]$Destination) {
    $null = [IO.Directory]::CreateDirectory((Split-Path $Destination -Parent))
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function CheckClosed([string]$Root) {
    $prefix = $Root.TrimEnd('\','/') + '\'
    foreach ($process in Get-Process -ErrorAction Stop) {
        if ($process.ProcessName -ieq 'SSFIV') { throw 'Close Ultra Street Fighter IV before upgrading.' }
        if ($process.ProcessName -in 'Launcher','Updater','sf4-net','ember-discord') {
            try { $path = $process.Path } catch { throw 'Close Ember and its helper processes before upgrading.' }
            if (!$path -or $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Close Ember and its helper processes before upgrading.'
            }
        }
    }
}

$packageRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\','/')
$payload = Join-Path $packageRoot 'payload'
$metadataPath = Join-Path $packageRoot 'upgrade.json'
$upgradeManifestPath = Join-Path $packageRoot 'UPGRADE_MANIFEST.txt'
$baseManifestPath = Join-Path $packageRoot 'base-manifest.txt'
$nextManifestPath = SafePath $payload 'MANIFEST.txt'
$metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
if ($metadata.schema -ne 1 -or $metadata.from -notmatch '^\d+\.\d+\.\d+$' -or $metadata.to -notmatch '^\d+\.\d+\.\d+$') {
    throw 'Unsupported upgrade metadata.'
}
if ((Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash -ine $metadata.installerSha256) {
    throw 'Upgrade installer is damaged.'
}
$upgradeInventory = ReadManifest $upgradeManifestPath $packageRoot
VerifyFiles $packageRoot $upgradeInventory
if ((Get-FileHash -LiteralPath $baseManifestPath -Algorithm SHA256).Hash -ine $metadata.baseManifestSha256 -or
    (Get-FileHash -LiteralPath $nextManifestPath -Algorithm SHA256).Hash -ine $metadata.targetManifestSha256) {
    throw 'Upgrade manifest is damaged.'
}
$before = ReadManifest $baseManifestPath $packageRoot
$after = ReadManifest $nextManifestPath $payload
$before['MANIFEST.txt'] = $metadata.baseManifestSha256
$after['MANIFEST.txt'] = $metadata.targetManifestSha256
$changed = @($after.Keys | Where-Object { !$before.ContainsKey($_) -or $before[$_] -ine $after[$_] } | Sort-Object)
$removed = @($before.Keys | Where-Object { !$after.ContainsKey($_) } | Sort-Object)
if (!(SameList $changed $metadata.changedFiles) -or !(SameList $removed $metadata.removedFiles)) {
    throw 'Upgrade inventory does not match its manifests.'
}
$actualPayload = @(Get-ChildItem -LiteralPath $payload -File -Recurse | ForEach-Object { $_.FullName.Substring($payload.Length + 1) } | Sort-Object)
if (!(SameList $changed $actualPayload)) { throw 'Upgrade payload files do not match its declared inventory.' }
$payloadHashes = @{}; foreach ($relative in $changed) { $payloadHashes[$relative] = $after[$relative] }
VerifyFiles $payload $payloadHashes

if (!$InstallDir) {
    Add-Type -AssemblyName System.Windows.Forms
    $picker = New-Object System.Windows.Forms.FolderBrowserDialog
    $picker.Description = "Select your existing SF4 Ember Netplay $($metadata.from) folder containing Launcher.exe."
    $picker.ShowNewFolderButton = $false
    try {
        if ($picker.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Write-Host 'Upgrade cancelled.'; return }
        $InstallDir = $picker.SelectedPath
    } finally { $picker.Dispose() }
}
$install = (Resolve-Path -LiteralPath $InstallDir).Path.TrimEnd('\','/')
if ($install -ieq [IO.Path]::GetPathRoot($install).TrimEnd('\','/')) { throw 'Select the Ember application folder, not a drive root.' }
$null = SafePath $install 'Launcher.exe'
if ($packageRoot -ieq $install -or $packageRoot.StartsWith($install + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $install.StartsWith($packageRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Extract this upgrade into a separate folder outside the Ember installation.'
}
$installedManifest = SafePath $install 'MANIFEST.txt'
if ((Test-Path -LiteralPath $installedManifest -PathType Leaf) -and
    (Get-FileHash -LiteralPath $installedManifest).Hash -ieq $metadata.targetManifestSha256) {
    VerifyFiles $install $after; VerifyAbsent $install $removed
    Write-Host "SF4 Ember Netplay $($metadata.to) is already installed and verified."
    return
}
Write-Host "Checking the existing $($metadata.from) files..."
VerifyFiles $install $before
CheckClosed $install
foreach ($relative in @($changed + $removed)) {
    $path = SafePath $install $relative
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $stream.Dispose()
    }
}
if ($CheckOnly) { Write-Host "Upgrade check passed: $($changed.Count) files to replace and $($removed.Count) to remove; no files changed."; return }

$parent = Split-Path $install -Parent
$id = [Guid]::NewGuid().ToString('N')
$stage = Join-Path $parent ('.ember-upgrade-stage-' + $id)
$backup = Join-Path $parent ("Ember-backup-$($metadata.from)-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $id.Substring(0,8))
$null = [IO.Directory]::CreateDirectory($stage)
$touched = New-Object 'System.Collections.Generic.List[string]'
$backupMade = $false
try {
    Write-Host 'Preparing and validating the upgrade...'
    foreach ($relative in $after.Keys) {
        $source = if ($payloadHashes.ContainsKey($relative)) { SafePath $payload $relative } else { SafePath $install $relative }
        CopyOne $source (SafePath $stage $relative)
    }
    VerifyFiles $stage $after; VerifyAbsent $stage $removed
    & (SafePath $stage 'preflight.ps1') -PackageDir $stage
    CheckClosed $install; VerifyFiles $install $before
    $null = [IO.Directory]::CreateDirectory($backup)
    foreach ($relative in @($changed + $removed)) {
        if ($before.ContainsKey($relative)) { CopyOne (SafePath $install $relative) (SafePath $backup $relative) }
    }
    $backupHashes = @{}; foreach ($relative in @($changed + $removed)) {
        if ($before.ContainsKey($relative)) { $backupHashes[$relative] = $before[$relative] }
    }
    VerifyFiles $backup $backupHashes
    [ordered]@{installation=$install;from=$metadata.from;to=$metadata.to;changed=$changed;removed=$removed;createdUtc=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backup 'UPGRADE-BACKUP.json') -Encoding UTF8
    $backupMade = $true
    Write-Host 'Installing the verified files...'
    foreach ($relative in $removed) {
        $touched.Add($relative)
        $path = SafePath $install $relative
        if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
    }
    $writeOrder = @($changed | Where-Object { $_ -ne 'MANIFEST.txt' }) + @('MANIFEST.txt')
    foreach ($relative in $writeOrder) {
        $touched.Add($relative)
        CopyOne (SafePath $stage $relative) (SafePath $install $relative)
    }
    VerifyFiles $install $after; VerifyAbsent $install $removed
    Write-Host "Upgrade complete: SF4 Ember Netplay $($metadata.to) verified."
    Write-Host "Backup of replaced $($metadata.from) files: $backup"
    Write-Host 'You can now run Launcher.exe from your Ember folder.'
} catch {
    $failure = $_
    if ($backupMade -and $touched.Count) {
        Write-Warning 'Upgrade failed. Restoring replaced files from the backup.'
        try {
            foreach ($relative in $touched) {
                $destination = SafePath $install $relative
                if ($before.ContainsKey($relative)) { CopyOne (SafePath $backup $relative) $destination }
                elseif (Test-Path -LiteralPath $destination -PathType Leaf) { Remove-Item -LiteralPath $destination -Force }
            }
            VerifyFiles $install $before
            Write-Warning "The original $($metadata.from) package was restored and verified."
        } catch { Write-Warning "Automatic restore could not finish. Keep the backup at $backup. Restore error: $_" }
    }
    throw $failure
} finally {
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    $expectedLeaf = '.ember-upgrade-stage-' + $id
    if ($resolvedStage.StartsWith([IO.Path]::GetFullPath($parent).TrimEnd('\','/') + '\', [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path $resolvedStage -Leaf) -eq $expectedLeaf) {
        try {
            $null = SafePath $parent $expectedLeaf
            if (Test-Path -LiteralPath $resolvedStage) { Remove-Item -LiteralPath $resolvedStage -Recurse -Force }
        } catch { Write-Warning "Temporary staging folder retained: $resolvedStage" }
    }
}
