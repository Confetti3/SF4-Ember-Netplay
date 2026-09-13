$ErrorActionPreference = 'Stop'
function Get-SourceFingerprint([string]$SourceRoot) {
    $paths = @(& git -C $SourceRoot ls-files --cached --others --exclude-standard) | Sort-Object -Unique
    if ($LASTEXITCODE) { throw 'Source inventory failed' }
    $lines = foreach ($relative in $paths) {
        $path = Join-Path $SourceRoot $relative
        if (Test-Path -LiteralPath $path -PathType Leaf) { "$relative $((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash)" }
        else { "$relative DELETED" }
    }
    $designation = Join-Path (Split-Path $SourceRoot -Parent) 'build-target.json'
    if (Test-Path -LiteralPath $designation) { $lines += "WORKSPACE_TARGET $((Get-FileHash -LiteralPath $designation).Hash)" }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join "`n")))).Replace('-','') }
    finally { $sha.Dispose() }
}
function Assert-CMakeSource([string]$SourceRoot, [string]$BuildRoot) {
    $cache = Get-Content -LiteralPath (Join-Path $BuildRoot 'CMakeCache.txt')
    $homeLine = @($cache | Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' })
    if ($homeLine.Count -ne 1) { throw 'CMake source identity missing' }
    $homePath = [IO.Path]::GetFullPath($homeLine[0].Substring($homeLine[0].IndexOf('=')+1))
    if ($homePath -ne [IO.Path]::GetFullPath($SourceRoot)) { throw "CMake source mismatch: $homePath" }
}
function Get-DependencyReceipt([string]$SourceRoot, [string]$BuildRoot) {
    $root = Join-Path $BuildRoot 'dependencies/x86-windows-wchar-filenames'
    $abiPath = Join-Path $root 'share/ggpo/vcpkg_abi_info.txt'
    $abi = Get-Content -LiteralPath $abiPath
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'vcpkg-ports/ggpo') -File) {
        $expected = "$($file.Name) $((Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant())"
        if ($abi -cnotcontains $expected) { throw "GGPO artifact was not built from current port input: $($file.Name)" }
    }
    $artifacts = foreach ($relative in @('bin/GGPO.dll','lib/GGPO.lib','include/ggponet.h','share/ggpo/vcpkg_abi_info.txt')) {
        $path = Join-Path $root $relative
        [pscustomobject]@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}
    }
    [pscustomobject]@{root=$root;artifacts=@($artifacts);rustLockSha256=(Get-FileHash -LiteralPath (Join-Path $SourceRoot 'rust/sf4-net/Cargo.lock')).Hash}
}
function Assert-BuildReceipt([string]$SourceRoot, [string]$BuildRoot, [string]$StageRoot) {
    Assert-CMakeSource $SourceRoot $BuildRoot
    $receipt = Get-Content -LiteralPath (Join-Path $BuildRoot 'build-provenance.json') -Raw | ConvertFrom-Json
    if ($receipt.sourceRoot -ne $SourceRoot -or $receipt.buildRoot -ne $BuildRoot -or $receipt.stageRoot -ne $StageRoot) { throw 'Build receipt path mismatch' }
    if ($receipt.sourceFingerprint -ne (Get-SourceFingerprint $SourceRoot)) { throw 'Source changed since verified build; rebuild with scripts/build-current.ps1' }
    foreach ($binary in $receipt.binaries) {
        if ((Get-FileHash -LiteralPath (Join-Path $StageRoot $binary.path)).Hash -ne $binary.sha256) { throw "Staged binary changed: $($binary.path)" }
    }
    if (Test-Path -LiteralPath (Join-Path $SourceRoot 'vcpkg-ports/ggpo')) {
        if (!$receipt.dependencies) { throw 'Dependency provenance missing; rebuild current target' }
        $current = Get-DependencyReceipt $SourceRoot $BuildRoot
        if ($current.root -ne $receipt.dependencies.root -or $current.rustLockSha256 -ne $receipt.dependencies.rustLockSha256) { throw 'Dependency provenance changed' }
        foreach ($artifact in $receipt.dependencies.artifacts) {
            if ((Get-FileHash -LiteralPath $artifact.path).Hash -ne $artifact.sha256) { throw "Dependency artifact changed: $($artifact.path)" }
        }
        if ((Get-FileHash -LiteralPath (Join-Path $StageRoot 'GGPO.dll')).Hash -ne $current.artifacts[0].sha256) { throw 'Staged GGPO does not match designated dependency' }
    }
    return $receipt
}
