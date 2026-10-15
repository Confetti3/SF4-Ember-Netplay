# Hash-validated SF4 Ember Netplay incremental upgrade installer.
[CmdletBinding()]
param([string]$InstallDir = '', [switch]$CheckOnly, [switch]$RecoverOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Do not depend on inherited PSModulePath ordering when an extracted installer
# is launched across Windows PowerShell and PowerShell 7 hosts.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility') -ErrorAction Stop

function ValidRelative([string]$Relative) {
    return !([IO.Path]::IsPathRooted($Relative) -or $Relative -match '[:*?"<>|]' -or
        @($Relative -split '[\\/]' | Where-Object { $_ -in '', '.', '..' -or $_.EndsWith('.') -or $_.EndsWith(' ') }).Count)
}

function SafePath([string]$Root, [string]$Relative) {
    if (!(ValidRelative $Relative)) { throw "Invalid package path: $Relative" }
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

# A journal's absolute path, checked as written the way the native reader
# checks it: a drive or UNC root, then no empty, '.', '..' or otherwise invalid
# part and no linked folder, so nothing is normalized away before the location
# checks. Returns the full path.
function AbsolutePath([string]$Value) {
    if ($Value -match '^([A-Za-z]:)[\\/](.+)$') { return SafePath ($Matches[1] + '\') $Matches[2] }
    if ($Value -match '^[\\/]{2}([^\\/]+)[\\/]([^\\/]+)[\\/](.+)$' -and (ValidRelative $Matches[2])) {
        return SafePath "\\$($Matches[1])\$($Matches[2])\" $Matches[3]
    }
    throw "Invalid update path: $Value"
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
    $a = [string[]]@($Left | ForEach-Object { [string]$_ })
    $b = [string[]]@($Right | ForEach-Object { [string]$_ })
    [Array]::Sort($a, [StringComparer]::Ordinal)
    [Array]::Sort($b, [StringComparer]::Ordinal)
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

# Recovery follows the native updater's (PackageInstaller.cxx RecoverLocked)
# decision for every journal, so either reader leaves a folder the same way;
# src/tests/data/upgrade-recovery/fixtures.json holds both to that.
function FileHash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash }

# What is at a path: Absent, File or Directory. Only "not found" is absence; a
# path whose status cannot be read (permissions, sharing, I/O) is an error, as
# in the native reader, so recovery stops with everything left as it was.
function PathStatus([string]$Path) {
    # The exception is tested by its own type: Windows PowerShell was seen to
    # match a typed catch for "not found" to an access-denied failure.
    try { $attributes = [IO.File]::GetAttributes($Path) }
    catch {
        $failure = $_.Exception
        while ($failure -is [Management.Automation.MethodInvocationException] -and $failure.InnerException) { $failure = $failure.InnerException }
        if ($failure -is [IO.FileNotFoundException] -or $failure -is [IO.DirectoryNotFoundException]) { return 'Absent' }
        throw "Cannot read update file during recovery: $Path"
    }
    if ($attributes -band [IO.FileAttributes]::Directory) { return 'Directory' }
    return 'File'
}

# A journal field by its exact name, as the native reader looks it up, with
# its JSON value as parsed (an array stays an array); $null when absent.
function JournalField($Object, [string]$Name) {
    foreach ($property in $Object.PSObject.Properties) { if ($property.Name -ceq $Name) { return ,$property.Value } }
    return $null
}

# Moves a journal that will not be recovered out of the way, kept as evidence
# when it can be renamed and otherwise removed, as the native reader does, so
# its failure is reported once and later runs can proceed.
function SetAsideJournal([string]$Path) {
    $failedPath = $Path + '.failed'
    try {
        if ([IO.File]::Exists($failedPath)) { [IO.File]::Replace($Path, $failedPath, [NullString]::Value) }
        else { [IO.File]::Move($Path, $failedPath) }
        return
    } catch {}
    try { [IO.File]::Delete($Path) } catch {}
    if ((PathStatus $Path) -ne 'Absent') { throw 'Cannot clear the pending update transaction.' }
}

# Backup bytes that disagree with the journal can never be trusted; a missing
# backup matters only when a restoration needs it. A backup that cannot be
# read is an ordinary error, and the journal stays for a retry.
function ValidateBackups([string]$Backup, [hashtable]$Files, [bool]$RequireAll) {
    foreach ($file in $Files.Values) {
        if (!$file.Operation -or !$file.Prior) { continue }
        $source = SafePath $Backup $file.Relative
        if ((PathStatus $source) -ne 'File') {
            if ($RequireAll) { throw [IO.InvalidDataException]::new("The recovery backup is missing or damaged: $($file.Relative)") }
        } elseif ((FileHash $source) -ine $file.Prior) {
            throw [IO.InvalidDataException]::new("The recovery backup is missing or damaged: $($file.Relative)")
        }
    }
}

# Restores only the recorded operations, from a backup set already validated.
# A file already at its prior bytes is not written, as it may be held open.
function RestoreOperations([string]$Install, [string]$Backup, [hashtable]$Files, [string[]]$Order) {
    $operations = @($Order | ForEach-Object { $Files[$_] } | Where-Object { $_.Operation })
    foreach ($file in $operations) {
        if ((PathStatus (SafePath $Install $file.Relative)) -eq 'Directory') { throw "Recovery destination is not a file: $($file.Relative)" }
    }
    foreach ($file in $operations) {
        $destination = SafePath $Install $file.Relative
        if ($file.Prior) {
            if ((PathStatus $destination) -eq 'File' -and (FileHash $destination) -ieq $file.Prior) { continue }
            ReplaceOne (SafePath $Backup $file.Relative) $destination $file.Prior
        } elseif ((PathStatus $destination) -ne 'Absent') { Remove-Item -LiteralPath $destination -Force }
    }
    foreach ($file in $operations) {
        $destination = SafePath $Install $file.Relative
        if ($file.Prior) {
            if ((PathStatus $destination) -ne 'File' -or (FileHash $destination) -ine $file.Prior) { throw "Restored update file failed verification: $($file.Relative)" }
        } elseif ((PathStatus $destination) -ne 'Absent') {
            throw "A newly added update file could not be removed during recovery: $($file.Relative)"
        }
    }
}

function RecoverTransaction([string]$Install, [string]$Path, [switch]$InspectOnly) {
    $journalStatus = PathStatus $Path
    if ($journalStatus -eq 'Absent') { return $false }
    if ($journalStatus -ne 'File') { throw 'Invalid update transaction path.' }
    # A journal that cannot be read stays for a retry.
    $text = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    if (!$InspectOnly) { CheckClosed $Install }
    # Invalid evidence in a journal that is not rolling back is set aside before
    # any destination changes. An unfinished rollback keeps its journal.
    $validating = $true; $rollingBack = $false
    $invalid = 'The pending update transaction is invalid. Preserve the installation and backup for manual recovery.'
    try {
        # Exactly what the native reader accepts, checked before any decision:
        # an object root (ConvertFrom-Json would unroll an array around one),
        # fields by their exact names and JSON types, never coerced.
        if (!$text -or !$text.TrimStart().StartsWith('{')) { throw $invalid }
        $transaction = $text | ConvertFrom-Json
        $state = JournalField $transaction 'state'
        $rollingBack = $state -is [string] -and $state -ceq 'rolling-back'
        $schema = JournalField $transaction 'schema'
        $operations = JournalField $transaction 'operations'
        $target = JournalField $transaction 'target'
        $backup = JournalField $transaction 'backup'
        $installation = JournalField $transaction 'installation'
        if ($transaction -isnot [pscustomobject] -or ($schema -isnot [int] -and $schema -isnot [long]) -or $schema -ne 1 -or
            $installation -isnot [string] -or (AbsolutePath $installation) -ine $Install -or $state -isnot [string] -or $state -cnotin 'prepared','committed','rolling-back' -or
            $operations -isnot [array] -or $operations.Count -eq 0 -or $target -isnot [pscustomobject] -or $backup -isnot [string]) {
            throw $invalid
        }
        if ($InspectOnly) {
            Write-Host "Pending update recovery is required. Run: powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -InstallDir `"$Install`" -RecoverOnly"
            return $true
        }
        $backupRoot = AbsolutePath $backup
        $backupParent = Split-Path $backupRoot -Parent
        if ($backupParent -ine (Join-Path $Install '.ember-update-backups') -and
            !($backupParent -ieq (Split-Path $Install -Parent) -and (Split-Path $backupRoot -Leaf) -clike 'Ember-backup-*')) {
            throw 'Invalid update backup location.'
        }
        # Every target and operation path, keyed and ordered as the native reader
        # keys them, so both restore, and name a missing file, in the same order.
        $files = @{}
        foreach ($property in $target.PSObject.Properties) {
            $relative = $property.Name.Replace('/','\')
            $null = SafePath $Install $relative
            $key = $relative.ToLowerInvariant()
            if ($files.ContainsKey($key) -or $property.Value -isnot [string] -or $property.Value -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid target inventory.' }
            $files[$key] = [pscustomobject]@{Relative=$relative;Target=$property.Value;Prior='';Operation=$false;State='';Current=''}
        }
        foreach ($operation in $operations) {
            if ($operation -isnot [pscustomobject]) { throw 'Invalid recovery operation.' }
            $operationPath = JournalField $operation 'path'; $existed = JournalField $operation 'existed'; $prior = JournalField $operation 'priorSha256'
            if ($operationPath -isnot [string] -or $existed -isnot [bool] -or $prior -isnot [string]) { throw 'Invalid recovery operation.' }
            $relative = $operationPath.Replace('/','\')
            $null = SafePath $Install $relative
            $null = SafePath $backupRoot $relative
            $key = $relative.ToLowerInvariant()
            if (($files.ContainsKey($key) -and $files[$key].Operation) -or $key -in '.ember-update.lock','.ember-update-transaction-v1.json' -or
                $key -like '.ember-update-backups*') { throw 'Invalid recovery operation.' }
            if (($existed -and $prior -notmatch '^[a-fA-F0-9]{64}$') -or (!$existed -and $prior -ne '')) { throw 'Invalid prior hash.' }
            if (!$files.ContainsKey($key)) { $files[$key] = [pscustomobject]@{Relative=$relative;Target='';Prior='';Operation=$false;State='';Current=''} }
            $files[$key].Operation = $true
            if ($existed) { $files[$key].Prior = $prior }
        }
        $order = [string[]]@($files.Keys)
        [Array]::Sort($order, [StringComparer]::Ordinal)
        # Only a rollback carries a missing skipped file forward; any other
        # journal finds missing files by looking at the folder.
        $missing = ''
        $missingProperty = @($transaction.PSObject.Properties | Where-Object { $_.Name -ceq 'missingSkipped' })
        if ($rollingBack -and $missingProperty) {
            if ($missingProperty[0].Value -isnot [string]) { throw 'Invalid missing update file.' }
            $missing = $missingProperty[0].Value.Replace('/','\')
            $null = SafePath $Install $missing
            $key = $missing.ToLowerInvariant()
            if (!$files.ContainsKey($key) -or $files[$key].Operation) { throw 'Invalid missing update file.' }
        }
        $validating = $false
        if ($state -ceq 'committed') {
            # "committed" is written only after every file matched the target.
            # A file that differs or is gone now was changed afterwards: the
            # backup holds only older bytes, which would downgrade it, and
            # keeping the journal would block every later update. It is cleared.
            Remove-Item -LiteralPath $Path -Force
            Write-Host 'The completed update transaction was cleared.'
            return $true
        }
        $restored = $rollingBack
        if ($rollingBack) {
            ValidateBackups $backupRoot $files $true
            RestoreOperations $Install $backupRoot $files $order
        } else {
            # A prepared journal: look at every recorded file before deciding.
            $operationsComplete = $true; $targetComplete = $true; $replaced = $false
            foreach ($key in $order) {
                $file = $files[$key]
                $destination = SafePath $Install $file.Relative
                $status = PathStatus $destination
                if ($status -eq 'Directory') { $file.State = 'Different' }
                elseif ($status -eq 'Absent') { $file.State = 'Missing' }
                else {
                    try { $file.Current = FileHash $destination } catch { throw "Cannot read update file during recovery: $($file.Relative)" }
                    $file.State = if ($file.Target -and $file.Current -ieq $file.Target) { 'Matching' } else { 'Different' }
                }
                $atTarget = if ($file.Target) { $file.State -eq 'Matching' } else { $file.State -eq 'Missing' }
                $atPrior = if ($file.Prior) { $file.Current -and $file.Current -ieq $file.Prior } else { $file.State -eq 'Missing' }
                if ($file.Operation) { $operationsComplete = $operationsComplete -and $atTarget }
                if ($file.Target) { $targetComplete = $targetComplete -and $atTarget }
                # Bytes that are neither the update's nor the ones it replaced
                # mean the folder was replaced since; it is not rolled back.
                if ($file.State -eq 'Different' -and (!$file.Operation -or !$atPrior)) { $replaced = $true }
                if (!$file.Operation -and $file.State -eq 'Missing' -and !$missing) { $missing = $file.Relative }
            }
            if ($replaced) {
                ValidateBackups $backupRoot $files $false
                SetAsideJournal $Path
                Write-Host 'The folder changed after the interrupted update, so it was left as it is.'
                return $true
            }
            if (!$operationsComplete) {
                ValidateBackups $backupRoot $files $true
                # Persist the rollback before the first destination changes, so
                # either reader resumes it without classifying a partly restored
                # folder again. A skipped file that is gone has no backup.
                $transaction.state = 'rolling-back'
                $transaction.PSObject.Properties.Remove('missingSkipped')
                if ($missing) { $transaction | Add-Member -NotePropertyName missingSkipped -NotePropertyValue $missing.Replace('\','/') }
                WriteTransaction $Path $transaction
                $rollingBack = $true; $restored = $true
                RestoreOperations $Install $backupRoot $files $order
            } else { ValidateBackups $backupRoot $files $false }
        }
        if ($missing) {
            SetAsideJournal $Path
            throw "An update file is missing and has no backup; install the update again: $($missing.Replace('\','/'))"
        }
        Remove-Item -LiteralPath $Path -Force
        if ($restored) { Write-Host "The interrupted update was restored from $($transaction.backup)." }
        else { Write-Host 'The interrupted update had already finished and was verified.' }
        return $true
    } catch {
        if (!$InspectOnly -and !$rollingBack -and ($validating -or $_.Exception -is [IO.InvalidDataException])) {
            try { SetAsideJournal $Path } catch {}
        }
        throw
    }
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
$changed = [string[]]@($after.Keys | Where-Object { !$before.ContainsKey($_) -or $before[$_] -ine $after[$_] })
$removed = [string[]]@($before.Keys | Where-Object { !$after.ContainsKey($_) })
[Array]::Sort($changed, [StringComparer]::Ordinal)
[Array]::Sort($removed, [StringComparer]::Ordinal)
if (!(SameList $changed $metadata.changedFiles) -or !(SameList $removed $metadata.removedFiles)) {
    throw 'Upgrade inventory does not match its manifests.'
}
$actualPayload = [string[]]@(Get-ChildItem -LiteralPath $payload -File -Recurse | ForEach-Object { $_.FullName.Substring($payload.Length + 1) })
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
$backup = Join-Path $parent ("Ember-backup-$($metadata.from)-" + (Get-Date).ToString('yyyyMMdd-HHmmss', [cultureinfo]::InvariantCulture) + '-' + $id.Substring(0,8))
$null = [IO.Directory]::CreateDirectory($stage)
$touched = New-Object 'System.Collections.Generic.List[string]'
$backupMade = $false; $committed = $false
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
    $committed = $true
    Remove-Item -LiteralPath $transactionPath -Force
    Write-Host "Upgrade complete: SF4 Ember Netplay $($metadata.to) verified."
    Write-Host "Backup of replaced $($metadata.from) files: $backup"
    Write-Host 'You can now run Launcher.exe from your Ember folder.'
} catch {
    $failure = $_
    if ($committed) {
        Write-Warning 'The upgrade was installed and verified; clearing its transaction is pending.'
    } elseif ($backupMade -and $touched.Count) {
        Write-Warning 'Upgrade failed. Restoring replaced files from the backup.'
        try {
            if (Test-Path -LiteralPath $transactionPath -PathType Leaf) {
                # This install just failed, so its own writes are undone whatever
                # the folder looks like now: the rollback is persisted first and
                # recovery resumes it rather than classifying the folder.
                $transaction.state = 'rolling-back'; WriteTransaction $transactionPath $transaction
                $null = RecoverTransaction $install $transactionPath
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
} finally {
    if ($lock) { $lock.Dispose() }
}
