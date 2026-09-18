# Hash-validated SF4 Ember Netplay incremental upgrade installer.
[CmdletBinding()]
param([string]$InstallDir = '', [switch]$CheckOnly, [switch]$RecoverOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Do not depend on inherited PSModulePath ordering when an extracted installer
# is launched across Windows PowerShell and PowerShell 7 hosts.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility') -ErrorAction Stop

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

function ReplaceOne([string]$Source, [string]$Destination, [string]$ExpectedHash) {
    $null = [IO.Directory]::CreateDirectory((Split-Path $Destination -Parent))
    $temporary = $Destination + '.ember-' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $inputStream = [IO.File]::OpenRead($Source)
    try {
        $outputStream = [IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try { $inputStream.CopyTo($outputStream); $outputStream.Flush($true) } finally { $outputStream.Dispose() }
    } finally { $inputStream.Dispose() }
    if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ine $ExpectedHash) {
        throw "Temporary update file failed verification: $Destination"
    }
    if ([IO.File]::Exists($Destination)) { [IO.File]::Replace($temporary,$Destination,[NullString]::Value) }
    else { [IO.File]::Move($temporary,$Destination) }
    if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -ine $ExpectedHash) {
        throw "Installed update file failed verification: $Destination"
    }
}

function WriteTransaction([string]$Path, $Value) {
    $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $json = $Value | ConvertTo-Json -Depth 8
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
    $stream = [IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
    if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary,$Path,[NullString]::Value) }
    else { [IO.File]::Move($temporary,$Path) }
}

function RecoverTransaction([string]$Install, [string]$Path, [switch]$InspectOnly) {
    if (!(Test-Path -LiteralPath $Path)) { return $false }
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw 'Invalid update transaction path.' }
    $transaction = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($transaction.schema -ne 1 -or $transaction.installation -ine $Install -or !$transaction.backup -or !$transaction.operations -or
        $transaction.state -notin 'prepared','committed' -or $transaction.target -isnot [pscustomobject]) {
        throw 'The pending update transaction is invalid. Preserve the installation and backup for manual recovery.'
    }
    if ($InspectOnly) {
        Write-Host "Pending update recovery is required. Run: powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -InstallDir `"$Install`" -RecoverOnly"
        return $true
    }
    CheckClosed $Install
    $backupRoot = [IO.Path]::GetFullPath([string]$transaction.backup).TrimEnd('\','/')
    $backupParent = Split-Path $backupRoot -Parent
    if ($backupParent -ine (Join-Path $Install '.ember-update-backups') -and
        !($backupParent -ieq (Split-Path $Install -Parent) -and (Split-Path $backupRoot -Leaf) -clike 'Ember-backup-*')) {
        throw 'Invalid update backup location.'
    }
    $target = @{}
    foreach ($property in $transaction.target.PSObject.Properties) {
        $null = SafePath $Install $property.Name
        $key = $property.Name.Replace('/','\')
        if ($target.ContainsKey($key) -or [string]$property.Value -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid target inventory.' }
        $target[$key] = [string]$property.Value
    }
    $seen = @{}
    foreach ($operation in @($transaction.operations)) {
        $key = ([string]$operation.path).Replace('/','\')
        $destination = SafePath $Install $key
        $source = SafePath $backupRoot $key
        if ($seen.ContainsKey($key) -or $key -in '.ember-update.lock','.ember-update-transaction-v1.json' -or
            $key -like '.ember-update-backups*' -or $operation.existed -isnot [bool] -or
            (Test-Path -LiteralPath $destination -PathType Container)) { throw 'Invalid recovery operation.' }
        $seen[$key] = $true
        if (($operation.existed -and [string]$operation.priorSha256 -notmatch '^[a-fA-F0-9]{64}$') -or
            (!$operation.existed -and [string]$operation.priorSha256 -ne '')) { throw 'Invalid prior hash.' }
        if ($transaction.state -eq 'prepared' -and $operation.existed -and
            (!(Test-Path -LiteralPath $source -PathType Leaf) -or (Get-FileHash -LiteralPath $source).Hash -ine [string]$operation.priorSha256)) {
            throw "The recovery backup is missing or damaged: $key"
        }
    }
    if ($transaction.state -eq 'committed') {
        VerifyFiles $Install $target
        foreach ($key in $seen.Keys) {
            if (!$target.ContainsKey($key) -and (Test-Path -LiteralPath (SafePath $Install $key))) { throw "Obsolete file remains in committed update: $key" }
        }
        Remove-Item -LiteralPath $Path -Force
        Write-Host 'The completed update transaction was verified.'
        return $true
    }
    foreach ($operation in @($transaction.operations)) {
        $destination = SafePath $Install ([string]$operation.path)
        if ($operation.existed) {
            $source = SafePath ([string]$transaction.backup) ([string]$operation.path)
            if (!(Test-Path -LiteralPath $source -PathType Leaf) -or
                (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine [string]$operation.priorSha256) {
                throw "The recovery backup is missing or damaged: $($operation.path)"
            }
            ReplaceOne $source $destination ([string]$operation.priorSha256)
        } elseif (Test-Path -LiteralPath $destination -PathType Leaf) {
            Remove-Item -LiteralPath $destination -Force
        }
    }
    $restored = @{}
    foreach ($operation in @($transaction.operations)) {
        if ($operation.existed) { $restored[[string]$operation.path] = [string]$operation.priorSha256 }
        elseif (Test-Path -LiteralPath (SafePath $Install ([string]$operation.path))) {
            throw "A newly added update file could not be removed during recovery: $($operation.path)"
        }
    }
    VerifyFiles $Install $restored
    Remove-Item -LiteralPath $Path -Force
    Write-Host "The interrupted update was restored from $($transaction.backup)."
    return $true
}

function CheckClosed([string]$Root) {
    $prefix = $Root.TrimEnd('\','/') + '\'
    foreach ($process in Get-Process -ErrorAction Stop) {
        if ($process.ProcessName -ieq 'SSFIV') { throw 'Close Ultra Street Fighter IV before upgrading.' }
        if ($process.ProcessName -in 'Launcher','Updater','sf4-net','ember-discord') {
            try { $path = $process.Path } catch { throw 'Close Ember and its helper processes before upgrading.' }
            # A 32-bit PowerShell cannot read a 64-bit process's module path, and
            # sf4-net is 64-bit. WMI reports it for either. A process WMI no
            # longer lists has exited and holds no files; one that is listed
            # without a path still fails closed.
            if (!$path) {
                try { $listed = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.Id)" -ErrorAction Stop }
                catch { throw 'Close Ember and its helper processes before upgrading.' }
                if (!$listed) { continue }
                $path = $listed.ExecutablePath
            }
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
$lockPath = SafePath $install '.ember-update.lock'
$transactionPath = SafePath $install '.ember-update-transaction-v1.json'
$lock = $null
try {
    if (!$CheckOnly) { $lock = [IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None) }
} catch { throw 'Another Ember update or recovery is using this installation.' }
try {
if (RecoverTransaction $install $transactionPath -InspectOnly:$CheckOnly) {
    if ($CheckOnly -or $RecoverOnly) { return }
}
if ($RecoverOnly) { Write-Host 'No interrupted update transaction was found.'; return }
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
    if (Test-Path -LiteralPath $path -PathType Container) { throw "A package file collides with a directory: $relative" }
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $access = if ($CheckOnly) { [IO.FileAccess]::Read } else { [IO.FileAccess]::ReadWrite }
        $stream = [IO.File]::Open($path, [IO.FileMode]::Open, $access, [IO.FileShare]::None)
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
    $operations = @()
    foreach ($relative in @($changed + $removed)) {
        $destination = SafePath $install $relative
        if (Test-Path -LiteralPath $destination -PathType Container) { throw "A package file collides with a directory: $relative" }
        $existed = Test-Path -LiteralPath $destination -PathType Leaf
        $priorHash = if ($existed) { (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash } else { '' }
        if ($existed) { ReplaceOne $destination (SafePath $backup $relative) $priorHash }
        $operations += [ordered]@{path=$relative;existed=$existed;priorSha256=$priorHash;
            targetSha256=if($after.ContainsKey($relative)){$after[$relative]}else{''}}
    }
    $backupHashes = @{}; foreach ($operation in $operations) {
        if ($operation.existed) { $backupHashes[$operation.path] = $operation.priorSha256 }
    }
    VerifyFiles $backup $backupHashes
    [ordered]@{installation=$install;from=$metadata.from;to=$metadata.to;changed=$changed;removed=$removed;createdUtc=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backup 'UPGRADE-BACKUP.json') -Encoding UTF8
    $backupMade = $true
    $targetTransaction = @{}; foreach($relative in $after.Keys){$targetTransaction[$relative]=$after[$relative]}
    $transaction = [ordered]@{schema=1;state='prepared';installation=$install;backup=$backup;
        from=$metadata.from;to=$metadata.to;operations=$operations;target=$targetTransaction;createdUtc=[DateTime]::UtcNow.ToString('o')}
    WriteTransaction $transactionPath $transaction
    Write-Host 'Installing the verified files...'
    foreach ($relative in $removed) {
        $touched.Add($relative)
        $path = SafePath $install $relative
        if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
    }
    $writeOrder = @($changed | Where-Object { $_ -ne 'MANIFEST.txt' }) + @('MANIFEST.txt')
    $completedWrites = 0
    foreach ($relative in $writeOrder) {
        $touched.Add($relative)
        ReplaceOne (SafePath $stage $relative) (SafePath $install $relative) $after[$relative]
        ++$completedWrites
        if ($env:SF4E_UPDATE_TEST_TERMINATE_AFTER -and $completedWrites -eq [int]$env:SF4E_UPDATE_TEST_TERMINATE_AFTER) {
            Stop-Process -Id $PID -Force
        }
    }
    VerifyFiles $install $after; VerifyAbsent $install $removed
    $transaction.state='committed'; WriteTransaction $transactionPath $transaction
    Remove-Item -LiteralPath $transactionPath -Force
    Write-Host "Upgrade complete: SF4 Ember Netplay $($metadata.to) verified."
    Write-Host "Backup of replaced $($metadata.from) files: $backup"
    Write-Host 'You can now run Launcher.exe from your Ember folder.'
} catch {
    $failure = $_
    if ($backupMade -and $touched.Count) {
        Write-Warning 'Upgrade failed. Restoring replaced files from the backup.'
        try {
            if (Test-Path -LiteralPath $transactionPath -PathType Leaf) { $null = RecoverTransaction $install $transactionPath }
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
} finally {
    if ($lock) { $lock.Dispose() }
}
