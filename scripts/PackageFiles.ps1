# The hash-file formats of a package, written and read in one place. Import-safe: dot-source it.
# The updater, preflight and Install-Upgrade.ps1 read these bytes, so the formats do not vary:
#   MANIFEST.txt and UPGRADE_MANIFEST.txt   '<lowercase sha256>  <relative path>' per file, ordinal path order,
#                                           UTF-8 without a byte-order mark, one line per file
#   <file>.sha256                           '<lowercase sha256>  <file name>', ASCII, one line
# The encodings are pinned here: Set-Content -Encoding UTF8 writes a byte-order mark on Windows PowerShell 5.1
# and none on PowerShell 7, and these scripts may start under either.
# tester-preflight.ps1 and upgrade/Install-Upgrade.ps1 ship inside packages and run on their own, so they keep
# their own readers; scripts/test-package-files.ps1 checks that those accept what is written here.

function Get-FileSha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Resolve-PackagePath([string]$Path) {
    $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Write-PackageLines([string]$Path, [string[]]$Lines, [Text.Encoding]$Encoding) {
    [IO.File]::WriteAllLines((Resolve-PackagePath $Path), $Lines, $Encoding)
}

# Lists every file under $Directory (ordinal order of the full path) with its hash, then writes the list to
# $Directory\$Name. The list is made before the file exists, so a manifest never names itself.
function Write-HashManifest([string]$Directory, [string]$Name = 'MANIFEST.txt') {
    $Directory = $Directory.TrimEnd('\', '/')
    $paths = [string[]]@(Get-ChildItem -LiteralPath $Directory -File -Recurse | ForEach-Object { $_.FullName })
    [Array]::Sort($paths, [StringComparer]::Ordinal)
    $lines = [string[]]@(foreach ($path in $paths) {
        '{0}  {1}' -f (Get-FileSha256 $path), $path.Substring($Directory.Length + 1)
    })
    Write-PackageLines (Join-Path $Directory $Name) $lines ([Text.UTF8Encoding]::new($false))
}

# Reads a manifest into a relative-path to hash table. Malformed lines and duplicate paths are refused.
function Read-HashManifest([string]$Path) {
    $entries = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$' -or $entries.ContainsKey($Matches[2])) { throw "Malformed manifest: $Path" }
        $entries[$Matches[2]] = $Matches[1]
    }
    return $entries
}

# Writes <File>.sha256 and returns the hash.
function Write-Sha256Sidecar([string]$File) {
    $hash = Get-FileSha256 $File
    Write-PackageLines "$File.sha256" @("$hash  $([IO.Path]::GetFileName($File))") ([Text.Encoding]::ASCII)
    return $hash
}

# Checks the file against its sidecar (one line, '<64 hex>  <exact file name><extension>') and returns the
# lowercase hash.
function Assert-Sha256Sidecar([string]$Path, [string]$Extension = '.zip') {
    $full = [IO.Path]::GetFullPath($Path)
    if (!(Test-Path -LiteralPath $full -PathType Leaf) -or !(Test-Path -LiteralPath ($full + '.sha256') -PathType Leaf)) {
        throw "Package or SHA-256 sidecar missing: $full"
    }
    $line = (Get-Content -LiteralPath ($full + '.sha256') -Raw).Trim()
    if ($line -notmatch ('^([0-9a-fA-F]{64})  ([^\\/]+' + [regex]::Escape($Extension) + ')$') -or $Matches[2] -cne [IO.Path]::GetFileName($full)) {
        throw "Malformed package checksum sidecar: $full.sha256"
    }
    $actual = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
    if ($actual -ine $Matches[1]) { throw "Package checksum mismatch: $full" }
    return $actual.ToLowerInvariant()
}
