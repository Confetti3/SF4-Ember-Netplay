#Requires -Version 7
# A release's Breakpad symbols, from the build to the report server's store (server/ember-reports/deploy/README.md,
# Symbols). Both publishers make them the same way:
#   1. right after the build, Assert-ReceiptSymbols refuses a build without them (BuildProvenance.ps1);
#   2. once the release is packaged, Save-EmberSymbolArchive keeps the verified store, its build receipt and the
#      installer that installs it in one archive beside the package, sf4-ember-netplay-<label>-symbols.zip, which the
#      publication record pins by its SHA-256. <build>\symbols is replaced by the next -Symbols build; the archive
#      is the release's lasting copy;
#   3. Publish-EmberSymbols, the Symbols step of the publication (Complete-EmberStaging, NightlyOperations.ps1),
#      uploads the symbols from that archive only, so a retry or a later manual upload (upload-symbols.ps1) never
#      needs the build again. The installer it runs is the caller's: a publication retries with the archived copy,
#      a manual upload runs the checkout's current one, because the installer owns the whole store (its lock, the
#      manifest naming and the prune of every release's manifest) and an old archive must never apply old rules.
# Dot-sourced by NightlyOperations.ps1; callers also dot-source BuildProvenance.ps1 (Assert-ReceiptSymbols).

# The archive's own files beside the store's <debug file>/<DEBUGID>/<name>.sym entries.
$EmberSymbolManifest = 'symbols.json'
$EmberSymbolInstaller = 'install-symbols.sh'

function Assert-EmberSymbolLabel([string]$Channel, [string]$Label) {
    if ($Channel -cnotin @('stable', 'nightly')) { throw "Unknown symbol channel: $Channel" }
    if ($Label -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$') { throw "Not a release label: $Label" }
}

function Get-EmberSymbolArchivePath([string]$OutDirectory, [string]$Label) {
    Join-Path $OutDirectory "sf4-ember-netplay-$Label-symbols.zip"
}

# The installer of a checkout, with the line endings it runs with on the server.
function Get-EmberSymbolInstaller([string]$SourceRoot) {
    (Get-Content -LiteralPath (Join-Path $SourceRoot "server/ember-reports/deploy/$EmberSymbolInstaller") -Raw).Replace("`r`n", "`n")
}

# Writes the archive from the receipt's store and returns it as a pinned artifact (path, name, sha256). Every
# entry's SHA-256 is taken from the bytes written, and the manifest records them with the channel, the label and
# the whole build receipt. An archive is never replaced: one already there belongs to a release with this label.
function Save-EmberSymbolArchive([string]$SourceRoot, [object]$Receipt, [string]$Channel, [string]$Label, [string]$OutDirectory) {
    Assert-EmberSymbolLabel $Channel $Label
    $store = Assert-ReceiptSymbols $Receipt
    $path = Get-EmberSymbolArchivePath $OutDirectory $Label
    if (Test-Path -LiteralPath $path) { throw "Symbol archive already exists: $path" }
    $installer = Get-EmberSymbolInstaller $SourceRoot
    $partial = "$path.$([Guid]::NewGuid().ToString('N')).partial"
    try {
        $zip = [IO.Compression.ZipArchive]::new([IO.File]::Open($partial, 'CreateNew', 'ReadWrite', 'None'), 'Create')
        try {
            $write = {
                param($Name, [byte[]]$Bytes)
                $stream = $zip.CreateEntry($Name, 'Optimal').Open()
                try { $stream.Write($Bytes, 0, $Bytes.Length) } finally { $stream.Dispose() }
                [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
            }
            $files = @(foreach ($module in $Receipt.symbols.modules) {
                $sym = @(Get-ChildItem -LiteralPath (Join-Path $store $module) -File -Filter '*.sym')
                if ($sym.Count -ne 1) { throw "The symbol store holds no single symbol file for $module." }
                $name = "$module/$($sym[0].Name)"
                [ordered]@{path=$name;sha256=(& $write $name ([IO.File]::ReadAllBytes($sym[0].FullName)))}
            })
            & $write $EmberSymbolInstaller ([Text.UTF8Encoding]::new($false).GetBytes($installer)) | Out-Null
            $manifest = [ordered]@{channel=$Channel;label=$Label;files=$files;receipt=$Receipt} | ConvertTo-Json -Depth 10
            & $write $EmberSymbolManifest ([Text.UTF8Encoding]::new($false).GetBytes($manifest)) | Out-Null
        } finally { $zip.Dispose() }
        [IO.File]::Move($partial, $path)
    } finally { if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial } }
    Get-NightlyArtifact $path
}

# A pinned archive's bytes, read once and checked against the record, so nothing can change between the check and
# the use.
function Read-EmberSymbolArchive([object]$Archive) {
    $bytes = [IO.File]::ReadAllBytes($Archive.path)
    if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant() -cne $Archive.sha256) {
        throw "Prepared artifact changed: $($Archive.path)"
    }
    , $bytes
}

# The installer a pinned archive carries: what a publication retries with, so one release runs one installer.
function Get-EmberArchivedSymbolInstaller([object]$Archive) {
    $zip = [IO.Compression.ZipArchive]::new([IO.MemoryStream]::new((Read-EmberSymbolArchive $Archive), $false), 'Read')
    try {
        $entry = $zip.GetEntry($EmberSymbolInstaller)
        if (!$entry) { throw "Not a symbol archive: $($Archive.path)" }
        $reader = [IO.StreamReader]::new($entry.Open(), [Text.UTF8Encoding]::new($false, $true), $false)
        try { $reader.ReadToEnd() } finally { $reader.Dispose() }
    } finally { $zip.Dispose() }
}

# Unpacks a pinned archive into a new private folder and returns it: the folder and the store's top-level
# <debug file> folders. It must be this channel's and label's, and hold exactly the files its manifest lists, each
# with its recorded SHA-256. The caller removes the folder.
function Expand-EmberSymbolArchive([object]$Archive, [string]$Channel, [string]$Label) {
    Assert-EmberSymbolLabel $Channel $Label
    $zip = [IO.Compression.ZipArchive]::new([IO.MemoryStream]::new((Read-EmberSymbolArchive $Archive), $false), 'Read')
    try {
        $read = {
            param($Entry)
            $buffer = [IO.MemoryStream]::new()
            $stream = $Entry.Open()
            try { $stream.CopyTo($buffer) } finally { $stream.Dispose() }
            , $buffer.ToArray()
        }
        $entries = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
        foreach ($entry in $zip.Entries) { $entries[$entry.FullName] = $entry }
        if (!$entries.ContainsKey($EmberSymbolManifest) -or !$entries.ContainsKey($EmberSymbolInstaller)) { throw "Not a symbol archive: $($Archive.path)" }
        $strict = [Text.UTF8Encoding]::new($false, $true)
        $manifest = $strict.GetString((& $read $entries[$EmberSymbolManifest])) | ConvertFrom-Json
        if ($manifest.channel -cne $Channel -or $manifest.label -cne $Label) {
            throw "The symbol archive is for $($manifest.channel) $($manifest.label), not $Channel $Label."
        }
        $listed = @($manifest.files | ForEach-Object path)
        if (!$listed.Count -or $entries.Count -ne $listed.Count + 2 -or @($listed | Where-Object { !$entries.ContainsKey($_) }).Count) {
            throw "The symbol archive's files differ from its manifest: $($Archive.path)"
        }
        $directory = Join-Path ([IO.Path]::GetTempPath()) "ember-symbols-$([Guid]::NewGuid().ToString('N'))"
        New-Item -ItemType Directory -Path $directory | Out-Null
        try {
            foreach ($file in $manifest.files) {
                if ($file.path -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_.+-]*/[0-9A-Fa-f]+/[A-Za-z0-9][A-Za-z0-9_.+-]*\.sym$') { throw "Unexpected symbol archive entry: $($file.path)" }
                $content = & $read $entries[$file.path]
                if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($content)).ToLowerInvariant() -cne $file.sha256) {
                    throw "Symbol archive entry changed: $($file.path)"
                }
                $target = Join-Path $directory $file.path
                New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
                [IO.File]::WriteAllBytes($target, $content)
            }
            [pscustomobject]@{directory=$directory; folders=@(Get-ChildItem -LiteralPath $directory -Directory | ForEach-Object FullName)}
        } catch { Remove-Item -LiteralPath $directory -Recurse -Force; throw }
    } finally { $zip.Dispose() }
}

# The Symbols step: installs the archive's symbols in the server's store with `$Installer`, as katie over ssh
# (install-symbols.sh: prepare an upload folder inside the store and print its path, scp into it, commit under the
# store's lock, clean up). The caller chooses the installer: the archived copy for a publication, the checkout's
# current one for a manual upload (see the header). Installing is idempotent, so a retry after any failure
# repeats it whole.
function Publish-EmberSymbols([object]$Archive, [string]$Channel, [string]$Label, [string]$Installer, [string]$Server = 'vps') {
    $contents = Expand-EmberSymbolArchive $Archive $Channel $Label
    try {
        Invoke-EmberStagedUpload $Server "$Channel $Label" $Installer $contents.folders '' "$Channel $Label symbols" | Out-Host
    } finally { Remove-Item -LiteralPath $contents.directory -Recurse -Force }
    Write-Host "Symbols for $Channel $Label are on ${Server}."
}